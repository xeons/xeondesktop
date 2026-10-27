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
import json
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
# user namespace); "fx", whether it needs xd-compositor-fx drawing with
# OpenGL ES (fx_environment).
SCENARIO_OPTIONS = {}


def scenario(desktop_ini=CLASSIC_INI, requires=(), outputs=None, setup=None, x11=False,
             showcase=False, fx=False):
    """Registers a scenario: a function of the session (Session), run with
    that desktop.ini; it's skipped without the Python modules it requires.
    A showcase one takes a README screenshot (tools/screenshots.sh) and
    runs only when named, never in the suite. An fx one runs on
    xd-compositor-fx (Desktop Effects' OpenGL effects), and is skipped
    where that can't draw."""

    def register(fn):
        SCENARIOS[fn.__name__] = (fn, desktop_ini, requires)
        SCENARIO_OPTIONS[fn.__name__] = {
            "outputs": outputs or [(WIDTH, HEIGHT, 1)],
            "setup": setup,
            "x11": x11,
            "showcase": showcase,
            "fx": fx,
        }
        return fn

    return register


class Failure(Exception):
    pass


class Skip(Exception):
    """The scenario can't run here (said why); not a failure."""


def fx_environment():
    """What an fx scenario's session gets: OpenGL ES, so xd-compositor
    hands over to xd-compositor-fx. Without a GPU of its own
    (XD_UI_RENDERER=gles2 asks for the machine's), Mesa's software
    rasterizer, llvmpipe, through EGL on no device at all
    (WLR_RENDERER_FORCE_SOFTWARE), buffers coming from /dev/udmabuf: the
    same pictures on every machine, and no GPU of the machine's touched.
    XD_VM_GPU: in a virtual machine too, where xd-compositor would
    otherwise keep to pixman."""
    env = {"WLR_RENDERER": "gles2", "XD_VM_GPU": "1"}
    if os.environ.get("XD_UI_RENDERER") != "gles2":
        env["WLR_RENDERER_FORCE_SOFTWARE"] = "1"
    return env


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


STANDIN_NAME = "User"
STANDIN_HOST = "xeondesktop"


def standin_identity():
    """/etc/passwd with our full name as STANDIN_NAME, and /etc/hostname."""
    uid = os.getuid()
    lines = []
    with open("/etc/passwd") as f:
        for line in f:
            fields = line.rstrip("\n").split(":")
            if len(fields) == 7 and fields[2] == str(uid):
                fields[4] = STANDIN_NAME
            lines.append(":".join(fields))
    return {"/etc/passwd": "\n".join(lines) + "\n", "/etc/hostname": STANDIN_HOST + "\n"}


def run_scenario(name, build, keep, keep_all=False):
    """Runs one scenario in a session of its own: "pass", "fail" or
    "skip"."""
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
        and k not in ("XDG_CURRENT_DESKTOP", "XMODIFIERS", "GDK_BACKEND", "XD_GLASS", "XD_NO_FX")
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
            # XD_UI_RENDERER=gles2 runs a scenario on the machine's GPU
            # (xd-compositor-fx, Aero's glass); CI has none.
            "WLR_RENDERER": os.environ.get("XD_UI_RENDERER", "pixman"),
            "WLR_HEADLESS_OUTPUTS": str(len(outputs)),
            **(fx_environment() if SCENARIO_OPTIONS[name]["fx"] else {}),
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
    # A showcase's screenshots are published: a stand-in for whoever runs
    # it (their full name in the passwd file, and the computer's name),
    # in this namespace only.
    showcase = SCENARIO_OPTIONS[name]["showcase"]
    if showcase:
        for path, text in standin_identity().items():
            fake = os.path.join(base, os.path.basename(path))
            with open(fake, "w") as f:
                f.write(text)
            binds.append(f"mount --bind {shlex.quote(fake)} {shlex.quote(path)}")
        binds.append(f"hostname {STANDIN_HOST}")
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
        "unshare", "-r", "-m", *(["--uts"] if showcase else []), "sh", "-c", script,
        "timeout", "-k", "5", str(SCENARIO_LIMIT),
        "dbus-run-session", "--",
        sys.executable, os.path.abspath(__file__), "--inner", name,
        "--build-dir", build, "--base", base, "--outer", str(os.getpid()),
    ]  # fmt: skip

    log_path = os.path.join(base, "log")
    passed = False
    skipped = False
    proc = None
    try:
        with open(log_path, "wb") as log:
            proc = subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                rc = proc.wait(timeout=SCENARIO_LIMIT + 15)
            except subprocess.TimeoutExpired:
                rc = None
        passed = rc == 0
        skipped = rc == SKIP
        with open(log_path, "rb") as f:
            out = f.read().decode(errors="replace")
        if skipped:
            sys.stdout.write("".join(l + "\n" for l in out.splitlines() if l.startswith("SKIPPED")))
        elif not passed:
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
    return "pass" if passed else "skip" if skipped else "fail"


def suite():
    """The scenarios run by default and by meson: all but the showcase."""
    return [n for n in SCENARIOS if not SCENARIO_OPTIONS[n]["showcase"]]


def outer(args):
    build = os.path.realpath(args.build_dir)
    names = args.scenarios or suite()
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
            fx = os.path.join(build, "xd-compositor-fx")
            if SCENARIO_OPTIONS[n]["fx"] and not os.access(fx, os.X_OK):
                print(f"SKIP {n} (xd-compositor-fx isn't built: SceneFX)", flush=True)
                skipped.append(n)
                continue
            start = time.monotonic()
            result = run_scenario(n, build, args.keep, args.keep_all)
            print(f"{result.upper()} {n} ({time.monotonic() - start:.1f}s)", flush=True)
            if result == "fail":
                failed.append(n)
            elif result == "skip":
                skipped.append(n)
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
    except Skip as e:
        print(f"SKIPPED: {e}", flush=True)
        rc = SKIP
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


def glass_setup(s):
    # What xd-compositor-fx sets on a GPU: glass frames, unblurred here.
    os.environ["XD_GLASS"] = "1"


def red_share(shot, rect):
    """The share (0..1) of pixels in rect that are clearly red."""
    x0, y0, w, h = rect
    red = 0
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            r, g, b = shot.pixel(x, y)
            red += r > g + 20 and r > b + 20
    return red / (w * h)


@scenario(AERO_INI, setup=glass_setup)
def aero_glass_caption_buttons(s):
    """On glass, a window's Close button is red from top to bottom (its
    measurements once came from wrongly un-premultiplied images, which
    turned its light rows teal), and red when hot; an inactive window's is
    clear glass, as Windows 7's, until hot."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.inject("key", "super+Right")  # the caption buttons at the screen's top right
    shot = s.settle("snapped")
    # Close's face, rows 3 to 17, either side of its glyph.
    sides = ((WIDTH - 49, 3, 8, 15), (WIDTH - 19, 3, 8, 15))
    for name, rect in zip(("left", "right"), sides):
        share = red_share(shot, rect)
        s.check(share > 0.9, f"Close is red down its {name} side ({share:.2f})")
    s.inject("move", WIDTH - 30, 10)
    hot = s.settle("close-hot")
    share = min(red_share(hot, rect) for rect in sides)
    s.check(share > 0.9, f"a hot Close is red ({share:.2f})")
    s.inject("move", WIDTH // 4, HEIGHT // 2)
    s.launch("xd-desk")
    s.wait_window(title="Display Properties", state="activated")
    s.wait(lambda: "activated" not in s.window(title="Control Panel")["states"],
           "Control Panel to be inactive")  # fmt: skip
    inactive = s.settle("inactive")
    share = max(red_share(inactive, rect) for rect in sides)
    s.check(share < 0.1, f"an inactive window's Close isn't red ({share:.2f})")
    s.inject("move", WIDTH - 30, 10)
    inactive_hot = s.settle("inactive-close-hot")
    share = min(red_share(inactive_hot, rect) for rect in sides)
    s.check(share > 0.9, f"an inactive window's hot Close is red ({share:.2f})")


def effects_setup(text):
    """A setup writing effects.ini (Desktop Effects) before the compositor
    starts."""

    def setup(s):
        os.makedirs(s.config, exist_ok=True)
        with open(os.path.join(s.config, "effects.ini"), "w") as f:
            f.write(text)

    return setup


def effects_support():
    """What the compositor told Desktop Effects it can draw (its support
    file), or None."""
    path = os.path.join(os.environ["XDG_RUNTIME_DIR"], "xeondesktop",
                        f"effects-{os.environ.get('WAYLAND_DISPLAY', 'wayland-0')}.ini")
    c = configparser.ConfigParser(interpolation=None)
    c.optionxform = str
    c.read(path)
    return dict(c["Support"]) if c.has_section("Support") else None


def require_opengl_effects(s):
    """Skips unless xd-compositor-fx is drawing with OpenGL ES."""
    support = effects_support()
    if support is None or support.get("OpenGL") != "true":
        why = support.get("Reason") if support else "no support file"
        raise Skip(f"xd-compositor-fx can't draw OpenGL effects here: {why}")
    s.log(f"OpenGL effects on {support.get('Renderer')}")


def left_edge(shot, empty, y):
    """The leftmost x where row y differs from the empty desktop, or None."""
    for x in range(WIDTH):
        if max(abs(a - b) for a, b in zip(shot.pixel(x, y), empty.pixel(x, y))) > 8:
            return x
    return None


def check_wobbles(s, empty, box):
    """Drags the window at box (x, y, w, h) quickly right by its title bar:
    mid-drag it bends, its bottom lagging behind its top, so its left edge
    isn't straight; let go, it settles into its rectangle again where it
    was dropped."""
    x, y, w, h = box
    rows = (y + 40, y + h // 2, y + h - 12)
    start = s.shot("before")
    before = [left_edge(start, empty, row) for row in rows]
    s.check(before.count(before[0]) == 3, f"the window starts square ({before})")
    # Right along the caption, 300 px in about half a second (xd-inject
    # waits for each move to be handled), then held still.
    cx, cy = x + w // 2, y + 10
    steps = ["move", cx, cy, "down", "sleep", 150]
    for i in range(1, 21):
        steps += ["move", cx + 15 * i, cy]
    steps += ["sleep", 1500, "up"]
    drag = s.launch(s.tool("xd-inject"), *map(str, steps))
    # Pictures while it moves (up to ten, till it's bent clearly): the
    # most bent one counts.
    time.sleep(0.3)
    bent = []
    for _ in range(10):
        if drag.poll() is not None:
            break
        shot = s.shot("dragging")
        edges = [left_edge(shot, empty, row) for row in rows]
        s.log(f"left edge mid-drag at rows {rows}: {edges}")
        if None not in edges:
            bent.append(edges)
            if max(edges) - min(edges) >= 8 and edges[2] < edges[0]:
                break
    edges = max(bent, key=lambda e: max(e) - min(e), default=[None])
    s.check(None not in edges and max(edges) - min(edges) >= 4,
            f"mid-drag the left edge isn't straight ({edges})")
    s.check(edges[2] < edges[0], f"its bottom lags behind its top ({edges})")
    drag.wait(timeout=20)
    dropped = s.settle("dropped")
    after = [left_edge(dropped, empty, row) for row in rows]
    s.check(after.count(after[0]) == 3 and abs(after[0] - (before[0] + 300)) <= 4,
            f"let go, it's square again where it was dropped ({after})")


@scenario(setup=effects_setup("[WobblyWindows]\nEnabled=true\n"), fx=True)
def wobbly_windows_drag(s):
    """Wobbly Windows (xd-compositor-fx): dragged quickly by its title bar,
    a window bends, its bottom lagging behind its top, so its left edge
    isn't straight mid-drag; let go, it settles into its rectangle again
    where it was dropped."""
    require_opengl_effects(s)
    empty = s.settle("desktop")
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    check_wobbles(s, empty, s.settle("window").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40)))


def effects_ini_value(s, group, key):
    return s.ini("effects.ini").get(group, key, fallback=None)


@scenario(fx=True)
def desktop_effects_panel(s):
    """Desktop Effects on xd-compositor-fx: typing in the Filter box finds
    Wobbly Windows and Enter opens its page; Use this effect (Space) turns
    it on, saved at once; Spring constant (Alt+S) set to 5.5 is saved; and
    the compositor applies it: the panel's own window wobbles when
    dragged."""
    require_opengl_effects(s)
    empty = s.settle("desktop")
    s.launch("xd-effects")
    s.wait_window(title="Desktop Effects", state="activated")
    grid = s.settle("grid")
    s.inject("type", "zzz", "sleep", 300)
    nothing = s.settle("nothing-found")
    s.check(nothing.differs(grid, (300, 80, 400, 60), step=2) > 0.05,
            "a filter finding nothing leaves no effect shown")
    s.inject("key", "ctrl+a", "type", "wobbly", "sleep", 300)
    found = s.settle("filtered")
    # Its category's first row, and nothing below it: the other effects
    # are filtered out.
    s.check(found.differs(nothing, (300, 80, 400, 60), step=2) > 0.05 and
            found.differs(nothing, (240, 140, 560, 300), step=2) < 0.01,
            "one finding Wobbly Windows shows it alone")
    s.inject("key", "Return")
    s.settle("page")
    s.inject("key", "space")
    s.wait(lambda: effects_ini_value(s, "WobblyWindows", "Enabled") == "true",
           "Wobbly Windows turned on in effects.ini")
    s.log("ok: Use this effect saved Enabled=true")
    s.inject("key", "alt+s", "sleep", 200, "key", "ctrl+a", "type", "5.5", "key", "Tab")
    s.wait(lambda: effects_ini_value(s, "WobblyWindows", "SpringConstant") == "5.5",
           "SpringConstant=5.5 in effects.ini")
    s.log("ok: Spring constant saved")
    s.check(effects_ini_value(s, "WobblyWindows", "Friction") is None,
            "options left at their defaults aren't written")
    page = s.settle("page-set")
    check_wobbles(s, empty, page.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40)))


@scenario()
def desktop_effects_unavailable(s):
    """Under the plain compositor (pixman) OpenGL effects are unavailable,
    and the compositor says why; Desktop Effects shows Wobbly Windows' page
    but can't turn it on (Use this effect is greyed out)."""
    support = effects_support()
    s.check(support is not None and support.get("OpenGL") == "false" and support.get("Reason"),
            f"the compositor says OpenGL effects are unavailable, and why ({support})")
    s.launch("xd-effects", "--effect", "WobblyWindows")
    s.wait_window(title="Desktop Effects", state="activated")
    s.settle("page")
    s.inject("key", "space", "sleep", 500)
    s.settle("after-space")
    s.check(effects_ini_value(s, "WobblyWindows", "Enabled") != "true",
            "Wobbly Windows stays off")


def row_span(shot, empty, y, left=0, right=WIDTH):
    """How wide row y's part that differs from the empty desktop is (from
    its first such pixel to its last), or 0."""
    xs = [x for x in range(left, right)
          if max(abs(a - b) for a, b in zip(shot.pixel(x, y), empty.pixel(x, y))) > 8]
    return xs[-1] - xs[0] + 1 if xs else 0


def open_control_panel(s, empty):
    """Control Panel opened and settled: its box (x, y, w, h)."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    return s.settle("window").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))


@scenario(setup=effects_setup("[MagicLamp]\nEnabled=true\nDuration=4000\n"), fx=True)
def magic_lamp_minimize(s):
    """Magic Lamp (xd-compositor-fx), slowed to 4 s: minimized, a window
    bends toward its taskbar button, so midway its rows near the taskbar
    are narrower than its top; then it's gone, minimized, and restoring it
    brings it back where it was."""
    require_opengl_effects(s)
    empty = s.settle("desktop")
    x, y, w, h = open_control_panel(s, empty)
    s.inject("key", "super+Down", "sleep", 1600)
    mid = s.shot("midway")
    top = row_span(mid, empty, y + 30)
    bottom = row_span(mid, empty, y + h - 10)
    s.log(f"midway: {top} px wide at its top, {bottom} near the taskbar (was {w})")
    s.check(top > w * 0.8, f"midway its top is still about as wide ({top} of {w})")
    s.check(0 < bottom < top * 0.75, f"near the taskbar it's narrower ({bottom} < {top})")
    s.wait_window(title="Control Panel", state="minimized")
    s.wait(lambda: s.shot("gone").differs(empty, (0, 0, WIDTH, HEIGHT - 40)) < 0.01,
           "the window gone into its button", timeout=10)
    s.inject("key", "alt+Tab", "sleep", 4600)
    back = s.settle("restored").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(back is not None and max(abs(a - b) for a, b in zip(back, (x, y, w, h))) <= 2,
            f"restored where it was ({back}, was {(x, y, w, h)})")


@scenario(setup=effects_setup("[MagicLamp]\nEnabled=true\nDuration=4000\n"
                              "[Animations]\nEnabled=true\nMinimizeEffect=fade\n"
                              "MinimizeDuration=4000\n"), fx=True)
def magic_lamp_over_animations(s):
    """Magic Lamp and Animations (Fade on minimize) both on, both slowed
    to 4 s: Magic Lamp alone minimizes the window. Midway its rows near the
    taskbar are narrower than its top, which a fading copy of the whole
    window (Animations as well) would fill; then it's gone."""
    require_opengl_effects(s)
    empty = s.settle("desktop")
    x, y, w, h = open_control_panel(s, empty)
    s.inject("key", "super+Down", "sleep", 1600)
    mid = s.shot("midway")
    top = row_span(mid, empty, y + 30)
    bottom = row_span(mid, empty, y + h - 10)
    s.log(f"midway: {top} px wide at its top, {bottom} near the taskbar (was {w})")
    s.check(top > w * 0.8, f"midway its top is still about as wide ({top} of {w})")
    s.check(0 < bottom < top * 0.75,
            f"near the taskbar it's narrower, no fading copy over it ({bottom} < {top})")
    s.wait_window(title="Control Panel", state="minimized")
    s.wait(lambda: s.shot("gone").differs(empty, (0, 0, WIDTH, HEIGHT - 40)) < 0.01,
           "the window gone into its button", timeout=10)


@scenario(setup=effects_setup("[Burn]\nEnabled=true\nDuration=4000\n"
                              "[Animations]\nEnabled=true\nCloseEffect=fade\n"
                              "CloseDuration=4000\n"), fx=True)
def burn_over_animations(s):
    """Burn and Animations (Fade on close) both on, both slowed to 4 s:
    Burn alone closes the window. Midway its top is gone, which a fading
    copy of the whole window (Animations as well) would still cover."""
    require_opengl_effects(s)
    empty = s.settle("desktop")
    x, y, w, h = open_control_panel(s, empty)
    s.inject("key", "alt+F4", "sleep", 1800)
    mid = s.shot("midway")
    top = mid.differs(empty, (x, y, w, 40), step=2)
    bottom = mid.differs(empty, (x, y + h - 60, w, 50), step=2)
    s.log(f"midway: {top:.2f} of its top differs from the desktop, {bottom:.2f} of its bottom")
    s.check(top < 0.05, f"midway its top is gone, no fading copy over it ({top:.2f})")
    s.check(bottom > 0.5, f"midway its bottom is still there ({bottom:.2f})")
    s.wait_window(title="Control Panel", gone=True)
    s.wait(lambda: s.shot("gone").differs(empty, (0, 0, WIDTH, HEIGHT - 40)) < 0.01,
           "the window burnt away", timeout=10)


@scenario(setup=effects_setup("[Burn]\nEnabled=true\nDuration=4000\n"), fx=True)
def burn_close(s):
    """Burn (xd-compositor-fx), slowed to 4 s: a window closed with Alt+F4
    burns away from the top down, so midway its top is gone (the desktop
    shows) while its bottom is still there; then it's all gone."""
    require_opengl_effects(s)
    empty = s.settle("desktop")
    x, y, w, h = open_control_panel(s, empty)
    s.inject("key", "alt+F4", "sleep", 1800)
    mid = s.shot("midway")
    top = mid.differs(empty, (x, y, w, 40), step=2)
    bottom = mid.differs(empty, (x, y + h - 60, w, 50), step=2)
    s.log(f"midway: {top:.2f} of its top differs from the desktop, {bottom:.2f} of its bottom")
    s.check(top < 0.05, f"midway its top is gone ({top:.2f})")
    s.check(bottom > 0.5, f"midway its bottom is still there ({bottom:.2f})")
    s.wait_window(title="Control Panel", gone=True)
    s.wait(lambda: s.shot("gone").differs(empty, (0, 0, WIDTH, HEIGHT - 40)) < 0.01,
           "the window burnt away", timeout=10)


@scenario(setup=effects_setup("[Explode]\nEnabled=true\nDuration=4000\n"), fx=True)
def explode_close(s):
    """Explode (xd-compositor-fx), slowed to 4 s: a window closed with
    Alt+F4 shatters, and midway its pieces lie outside its old box (below
    it and to its right, where the desktop was empty); then they're
    gone."""
    require_opengl_effects(s)
    empty = s.settle("desktop")
    x, y, w, h = open_control_panel(s, empty)
    s.inject("key", "alt+F4", "sleep", 1600)
    mid = s.shot("midway")
    right = mid.differs(empty, (x + w + 4, y, WIDTH - (x + w + 4), h), step=2)
    below = mid.differs(empty, (x, y + h + 4, w, HEIGHT - 40 - (y + h + 4)), step=2)
    inside = mid.differs(empty, (x, y, w, h), step=2)
    s.log(f"midway: {right:.2f} right of its box, {below:.2f} below it, {inside:.2f} inside")
    s.check(right > 0.03 and below > 0.03, f"pieces lie outside its box ({right:.2f}, {below:.2f})")
    s.check(inside < 0.9, f"its box isn't whole any more ({inside:.2f})")
    s.wait_window(title="Control Panel", gone=True)
    s.wait(lambda: s.shot("gone").differs(empty, (0, 0, WIDTH, HEIGHT - 40)) < 0.01,
           "the pieces gone", timeout=10)


@scenario(setup=effects_setup("[Water]\nEnabled=true\n"), fx=True)
def water_ripple(s):
    """Water Effect (xd-compositor-fx): Shift+Windows+click starts a
    ripple at the pointer, so the desktop's pixels around it (its icons)
    are displaced, while those far from it aren't; the click reaches
    nothing else, and the water settles flat again. Shift+F9 makes it
    rain till pressed again, and with Ctrl and the Windows key held the
    pointer leaves a wake."""
    require_opengl_effects(s)
    s.inject("move", 300, 400)
    empty = s.settle("desktop")
    near = (0, 30, 180, 180)
    far = (600, 300, 300, 300)
    s.inject("move", 70, 110, "hold", "super", "hold", "shift", "click",
             "release", "shift", "release", "super", "sleep", 150)
    rippling = s.shot("rippling")
    moved = rippling.differs(empty, near, step=2)
    still = rippling.differs(empty, far, step=2)
    s.log(f"{moved:.2f} of the pixels near the pointer moved, {still:.2f} far from it")
    s.check(moved > 0.05, f"the desktop near the pointer is displaced ({moved:.2f})")
    s.check(still < 0.01, f"far from it nothing moved yet ({still:.2f})")
    # Flat again and just as it was: the click selected no icon and
    # opened no Start menu.
    s.inject("move", 300, 400)
    s.wait(lambda: s.shot("settled").differs(empty, (0, 0, WIDTH, HEIGHT)) < 0.002,
           "the water flat again, nothing clicked", timeout=15)
    # Shift+F9: rain, drops all over the screen till it's pressed again
    # (faint rings on a plain desktop: they shade it a little).
    s.inject("key", "shift+F9", "sleep", 1500)
    raining = s.shot("raining").differs(empty, (0, 0, WIDTH, HEIGHT - 40), step=4, tolerance=2)
    s.check(raining > 0.005, f"Shift+F9 makes it rain ({raining:.2f} of the screen rippling)")
    s.inject("key", "shift+F9")
    s.wait(lambda: s.shot("rain-over").differs(empty, (0, 0, WIDTH, HEIGHT)) < 0.002,
           "the rain stopped and the water flat again", timeout=15)
    # Ctrl and the Windows key held, the pointer leaves a wake (and letting
    # go opens no Start menu).
    steps = ["move", 300, 400, "hold", "ctrl", "hold", "super", "sleep", 100]
    for i in range(1, 16):
        steps += ["move", 300 + 20 * i, 400]
    steps += ["release", "super", "release", "ctrl", "sleep", 100]
    s.inject(*map(str, steps))
    wake = s.shot("wake").differs(empty, (300, 340, 320, 120), step=2, tolerance=2)
    s.check(wake > 0.05, f"the pointer left a wake ({wake:.2f})")
    s.wait(lambda: s.shot("wake-over").differs(empty, (0, 0, WIDTH, HEIGHT)) < 0.002,
           "the wake settled, no Start menu", timeout=15)


# ---- the scene-graph Desktop Effects (both compositors) ---------------------------------


def darker(pixel, than, by=30):
    """Whether every channel of pixel is at least `by` darker than `than`'s
    (where `than` has that much to lose)."""
    return all(a <= b - min(by, b // 2) for a, b in zip(pixel, than))


def bright_runs(shot, background, y, tolerance=24):
    """The runs (x0, x1) of row y that differ from the background colour,
    at least 20 px long: thumbnails over the darkened desktop."""
    runs, start = [], None
    for x in range(WIDTH + 1):
        on = x < WIDTH and max(abs(a - b) for a, b in
                               zip(shot.pixel(x, y), background)) > tolerance
        if on and start is None:
            start = x
        elif not on and start is not None:
            if x - start >= 20:
                runs.append((start, x - 1))
            start = None
    return runs


def open_two_windows(s):
    """Control Panel and the Run box, Run in front."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.launch("xd-run")
    s.wait_window(title="Run", state="activated")
    return s.settle("two-windows")


def active_title(s):
    return next((w["title"] for w in s.windows() if "activated" in w["states"]), None)


@scenario(setup=effects_setup("[Scale]\nEnabled=true\n"))
def scale_pick_window(s):
    """Scale: Shift+Alt+Up lays the windows out side by side over the
    darkened desktop, smaller; a click on a thumbnail picks that window,
    which comes back in front and active; the top right corner starts it
    too, and Esc puts every window back as it was."""
    empty = s.settle("desktop")
    windows = open_two_windows(s)
    s.inject("key", "shift+alt+Up")
    grid = s.settle("grid")
    s.check(darker(grid.pixel(3, 3), empty.pixel(3, 3)),
            f"the desktop is darkened ({grid.pixel(3, 3)} against {empty.pixel(3, 3)})")
    runs = bright_runs(grid, grid.pixel(3, (HEIGHT - 40) // 2), (HEIGHT - 40) // 2)
    s.log(f"thumbnails across the middle: {runs}")
    s.check(len(runs) == 2, f"two thumbnails side by side ({runs})")
    s.check(grid.differs(windows, (0, 0, WIDTH, HEIGHT - 40)) > 0.2, "the windows moved")
    # The one that isn't Run.
    picked = None
    for x0, x1 in runs:
        s.inject("move", (x0 + x1) // 2, (HEIGHT - 40) // 2, "click")
        s.settle("picked")
        picked = active_title(s)
        if picked == "Control Panel":
            break
        s.inject("key", "shift+alt+Up")
        s.settle("grid-again")
    s.check(picked == "Control Panel", f"a click on its thumbnail picks Control Panel ({picked})")
    after = s.settle("after")
    s.check(not darker(after.pixel(3, 3), empty.pixel(3, 3), by=8),
            "the desktop isn't dark any more")
    # The corner, then Esc: as it was, Control Panel still in front.
    s.inject("move", WIDTH // 2, HEIGHT // 2, "move", WIDTH - 1, 0)
    corner = s.settle("corner")
    s.check(darker(corner.pixel(3, 3), empty.pixel(3, 3)), "the top right corner starts it")
    s.inject("key", "Escape")
    back = s.settle("escaped")
    s.check(back.differs(after, (0, 0, WIDTH, HEIGHT - 40)) < 0.02,
            "Esc puts the windows back as they were")
    s.check(active_title(s) == "Control Panel", "and Control Panel stays active")


def between(pixel, low, high, margin=12):
    """Whether each channel of pixel is between low's and high's, clear of
    both by margin where they differ enough."""
    clear = False
    for p, a, b in zip(pixel, low, high):
        lo, hi = min(a, b), max(a, b)
        if hi - lo >= 3 * margin:
            if not lo + margin <= p <= hi - margin:
                return False
            clear = True
    return clear


@scenario(setup=effects_setup(
    "[Animations]\nEnabled=true\nCloseEffect=fade\nCloseDuration=4000\n"))
def animations_close_fade(s):
    """Animations, Fade on close (slowed to four seconds): a closed
    window's last picture stays where it was and fades into the desktop,
    so a moment after Alt+F4 its pixels are a blend of its own and the
    desktop's; then it's gone."""
    empty = s.settle("desktop")
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    window = s.settle("window")
    box = window.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None, "the window shows")
    # A point inside the window far from the desktop's colour.
    x0, y0, w, h = box
    points = [(x, y) for y in range(y0 + 40, y0 + h - 10, 23) for x in range(x0 + 10, x0 + w - 10, 31)
              if sum(abs(a - b) for a, b in zip(window.pixel(x, y), empty.pixel(x, y))) > 200]
    s.check(len(points) > 10, f"the window has pixels unlike the desktop ({len(points)})")
    s.inject("key", "alt+F4")
    s.wait_window(title="Control Panel", gone=True)
    blended = 0
    for i in range(12):
        shot = s.shot(f"fading-{i}")
        mixed = sum(between(shot.pixel(x, y), window.pixel(x, y), empty.pixel(x, y))
                    for x, y in points)
        s.log(f"{mixed} of {len(points)} points blended")
        if mixed > len(points) * 0.8:
            blended += 1
            if blended >= 2:
                break
        time.sleep(0.25)
    s.check(blended >= 2, "mid-fade the closed window's pixels are blended with the desktop's")
    s.wait(lambda: s.shot("gone").differs(empty, (0, 0, WIDTH, HEIGHT - 40)) < 0.01,
           "the window to have faded away", timeout=10)


@scenario(setup=effects_setup("[Animations]\nEnabled=true\nMinimizeDuration=3000\n"
                              "UnminimizeDuration=1500\n"))
def animations_minimize_zoom(s):
    """Animations, Zoom on minimize (slowed down): Win+Down shrinks the
    window's picture towards its taskbar button, in place of XP's bar;
    restored from its button, it zooms back and is itself again."""
    empty = s.settle("desktop")
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    window = s.settle("window")
    box = window.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.inject("key", "super+Down")
    s.wait_window(title="Control Panel", state="minimized")
    time.sleep(1.2)
    mid = s.shot("zooming")
    now = mid.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.log(f"window {box}, mid-zoom {now}")
    s.check(now is not None and now[2] < box[2] - 20 and now[3] < box[3] - 20
            and now[1] > box[1], "mid-way it's smaller and lower, going to its button")
    s.wait(lambda: s.shot("minimized").differs(empty, (0, 0, WIDTH, HEIGHT - 40)) < 0.01,
           "the picture to reach the button", timeout=10)
    # Its taskbar button, from the taskbar's changes.
    button = s.shot("taskbar").changed_box(empty, (150, HEIGHT - 36, WIDTH - 300, 32))
    s.check(button is not None, f"its taskbar button shows ({button})")
    s.inject("move", button[0] + button[2] // 2, HEIGHT - 20, "click")
    s.wait_window(title="Control Panel", state="activated")
    s.inject("move", WIDTH - 20, 300)  # no taskbar thumbnail
    back = s.settle("restored")
    s.check(back.differs(window, (0, 0, WIDTH, HEIGHT - 40)) < 0.02,
            "restored, the window is itself again")


def contrasting_points(shot, empty, box, avoid=None):
    """Points in box where shot is far from the desktop's colour, not in
    the box `avoid`."""
    x0, y0, w, h = box
    points = []
    for y in range(y0 + 40, y0 + h - 10, 17):
        for x in range(x0 + 10, x0 + w - 10, 23):
            if avoid and avoid[0] - 4 <= x < avoid[0] + avoid[2] + 4 and \
                    avoid[1] - 4 <= y < avoid[1] + avoid[3] + 4:
                continue
            if sum(abs(a - b) for a, b in zip(shot.pixel(x, y), empty.pixel(x, y))) > 200:
                points.append((x, y))
    return points


def blended_share(shot, opaque, empty, points):
    return sum(between(shot.pixel(x, y), opaque.pixel(x, y), empty.pixel(x, y))
               for x, y in points) / max(len(points), 1)


@scenario(setup=effects_setup(
    "[WindowOpacity]\nEnabled=true\nDimInactive=true\nInactiveOpacity=50\n"))
def window_opacity_inactive(s):
    """Window Opacity: with inactive windows dimmed to 50%, Control Panel
    behind the Run box shows the desktop through it, while Run, active,
    is opaque; Alt and the wheel down over Run make it see-through too,
    and up again opaque."""
    empty = s.settle("desktop")
    s.inject("move", WIDTH - 20, 300)
    s.launch("xd-run")
    s.wait_window(title="Run", state="activated")
    run_box = s.settle("run").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    control = s.settle("control-panel")
    control_box = control.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.log(f"Control Panel {control_box}, Run {run_box}")
    s.inject("key", "alt+Tab")
    s.wait_window(title="Run", state="activated")
    both = s.settle("both")
    points = contrasting_points(control, empty, control_box, avoid=run_box)
    s.check(len(points) > 10, f"Control Panel has pixels unlike the desktop ({len(points)})")
    share = blended_share(both, control, empty, points)
    s.check(share > 0.8, f"inactive, Control Panel is blended with the desktop ({share:.2f})")
    run_points = contrasting_points(both, empty, run_box)
    s.check(blended_share(both, both, empty, run_points) == 0, "Run, active, is opaque")
    # Alt and the wheel over Run: 10 steps of 5% down.
    s.inject("move", run_box[0] + run_box[2] // 2, run_box[1] + 60, "hold", "alt",
             "wheel", 10, "release", "alt")
    faded = s.settle("run-faded")
    # What's behind Run (Control Panel, dimmed) now shows through it.
    share = faded.differs(both, run_box)
    s.check(share > 0.2, f"Alt and the wheel down make Run see-through ({share:.2f})")
    s.inject("hold", "alt", "wheel", -10, "release", "alt")
    opaque = s.settle("run-opaque")
    s.check(opaque.differs(both, run_box) < 0.02, "and up again, opaque")


@scenario(setup=effects_setup("[RingSwitcher]\nEnabled=true\n"))
def ring_switcher_thumbnails(s):
    """Ring Switcher: Super+Tab, the Windows key held, shows the windows'
    thumbnails over the darkened screen, the next window's in front with
    its title below; letting go of the Windows key switches to it."""
    empty = s.settle("desktop")
    s.inject("move", WIDTH - 20, 300)
    windows = open_two_windows(s)
    # One xd-inject holds the key throughout (its keyboard goes with it).
    held = s.launch(s.tool("xd-inject"), "hold", "super", "key", "Tab", "sleep", "4000",
                    "release", "super")
    time.sleep(1.0)
    ring = s.settle("ring", timeout=2.5)
    s.check(darker(ring.pixel(3, 3), empty.pixel(3, 3)), "the screen is darkened")
    s.check(ring.differs(windows, (0, 0, WIDTH, HEIGHT - 40)) > 0.2, "the thumbnails show")
    # The front thumbnail, below the middle, is bright on the dark screen.
    front = bright_runs(ring, ring.pixel(3, HEIGHT // 2 + 100), HEIGHT // 2 + 100)
    s.log(f"across the front: {front}")
    s.check(len(front) >= 1, f"the front thumbnail shows ({front})")
    s.check(active_title(s) == "Run", "Run is still active meanwhile")
    held.wait(timeout=20)
    s.wait(lambda: active_title(s) == "Control Panel", "Control Panel to be switched to")
    after = s.settle("after")
    s.check(not darker(after.pixel(3, 3), empty.pixel(3, 3), by=8), "the ring is gone")
    s.inject("key", "super+Tab")
    s.wait(lambda: active_title(s) == "Run", "Super+Tab alone to switch back to Run")


def zoomed_match(zoomed, before, x, y, level, radius=150, step=7):
    """Around (x, y): the share of points that show what `before` had
    `level` times closer to (x, y), and the share that changed at all.
    The pointer itself (below and right of it) isn't counted."""
    total = matched = changed = 0
    for dy in range(-radius, radius + 1, step):
        for dx in range(-radius, radius + 1, step):
            if -10 <= dx <= 75 and -10 <= dy <= 75:
                continue
            zx, zy = x + dx, y + dy
            bx, by = round(x + dx / level), round(y + dy / level)
            if not (0 <= zx < WIDTH and 0 <= zy < HEIGHT):
                continue
            total += 1
            z, b, same = zoomed.pixel(zx, zy), before.pixel(bx, by), before.pixel(zx, zy)
            matched += max(abs(p - q) for p, q in zip(z, b)) <= 48
            changed += max(abs(p - q) for p, q in zip(z, same)) > 24
    return matched / max(total, 1), changed / max(total, 1)


@scenario(setup=effects_setup("[EnhancedZoom]\nEnabled=true\nZoomFactor=2.0\n"))
def enhanced_zoom_pointer(s):
    """Enhanced Zoom Desktop (zoom factor 2): Windows and the wheel up
    zoom the screen in around the pointer: what was near it is twice as
    big and twice as far from it, the point under it staying put; the
    wheel down zooms back out to the screen as it was."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    x, y = 330, 260
    s.inject("move", x, y)
    before = s.settle("before")
    s.inject("hold", "super", "wheel", -1, "release", "super")
    zoomed = s.settle("zoomed")
    matched, changed = zoomed_match(zoomed, before, x, y, 2.0)
    s.log(f"{matched:.2f} of the points show what was half as far from the pointer; "
          f"{changed:.2f} changed")
    s.check(changed > 0.3, f"the screen is zoomed ({changed:.2f} of it changed)")
    s.check(matched > 0.8, f"what's round the pointer is twice the size ({matched:.2f})")
    s.inject("hold", "super", "wheel", 1, "release", "super")
    after = s.settle("unzoomed")
    s.check(after.differs(before, (0, 0, WIDTH, HEIGHT)) < 0.02, "zoomed out, it's as it was")


def is_red(pixel):
    r, g, b = pixel
    return r > 200 and g < 60 and b < 60


@scenario(setup=effects_setup("[Annotate]\nEnabled=true\n"))
def annotate_stroke(s):
    """Annotate: Windows+Alt and a left-button drag draw a red line (with a
    green outline, Compiz's colours) over the windows, which keep the
    focus; Windows+Alt+K clears it."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.inject("move", WIDTH - 20, 300)
    before = s.settle("before")
    steps = ["hold", "super", "hold", "alt", "move", 150, 200, "down"]
    for i in range(1, 11):
        steps += ["move", 150 + 40 * i, 200 + 20 * i]
    steps += ["up", "release", "alt", "release", "super", "move", WIDTH - 20, 300]
    s.inject(*steps)
    drawn = s.settle("drawn")
    on_line = [(150 + 40 * i, 200 + 20 * i) for i in range(0, 11)]
    red = sum(is_red(drawn.pixel(x, y)) for x, y in on_line)
    s.check(red >= 10, f"a red line along the drag ({red} of {len(on_line)} points)")
    green = sum(drawn.pixel(x, y + 2)[1] > 200 and drawn.pixel(x, y + 2)[0] < 100
                for x, y in on_line)
    s.check(green >= 5, f"with its green outline ({green})")
    s.check(drawn.differs(before, (0, 0, WIDTH, HEIGHT), step=2) < 0.05, "the rest is as it was")
    s.check(active_title(s) == "Control Panel", "the window keeps the focus")
    s.inject("key", "super+alt+k")
    cleared = s.settle("cleared")
    s.check(cleared.differs(before, (0, 0, WIDTH, HEIGHT)) < 0.01, "Windows+Alt+K clears it")


@scenario(setup=effects_setup("[ShowMouse]\nEnabled=true\n"))
def show_mouse_ring(s):
    """Show Mouse: Windows+K switches on a turning ring of glowing dots
    round the pointer, which follows it; Windows+K again switches it off."""
    x, y = 500, 350
    s.inject("move", x, y)
    before = s.shot("before")
    s.inject("key", "super+k", "sleep", 300)
    ring = s.shot("ring")
    around = (x - 60, y - 60, 120, 120)
    share = ring.differs(before, around, step=2)
    s.check(share > 0.1, f"pixels round the pointer change ({share:.2f})")
    s.check(ring.differs(before, (0, 0, x - 80, HEIGHT - 40)) == 0, "and nowhere else")
    time.sleep(0.3)
    turned = s.shot("turned")
    s.check(turned.differs(ring, around, step=2) > 0.02, "the ring turns")
    s.inject("move", 200, 200, "sleep", 300)
    moved = s.shot("moved")
    s.check(moved.differs(before, (140, 140, 120, 120), step=2) > 0.1, "it follows the pointer")
    s.inject("key", "super+k")
    off = s.settle("off")
    s.check(off.differs(before, (0, 0, WIDTH, HEIGHT - 40)) < 0.01, "Windows+K again hides it")
# ---- virtual desktops and their effects (General Options, Desktop Wall, Desktop Cube,
# Rotate Cube, Expo, Shift Switcher) ----------------------------------------------------

TWO_DESKTOPS = "[GeneralOptions]\nHorizontalSize=2\n"


def settle_and_open_control_panel(s):
    """The empty desktop settled, then Control Panel opened: the desktop's
    shot and the window's box on the screen (x, y, w, h)."""
    empty = s.settle("desktop")
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    return empty, s.settle("window").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))


@scenario(setup=effects_setup(TWO_DESKTOPS))
def virtual_desktops(s):
    """Two desktops side by side (General Options): Ctrl+Alt+Right shows
    the second, without the window opened on the first or its taskbar
    button; Ctrl+Alt+Left brings both back. Ctrl+Alt+Shift+Right carries
    the window along to the second desktop, so going back to the first
    leaves it behind. The taskbar's pager switches too."""
    empty, box = settle_and_open_control_panel(s)
    s.log(f"window at {box}")
    shown = s.settle("shown")
    s.inject("key", "ctrl+alt+Right")
    s.wait_window(title="Control Panel", gone=True)
    second = s.settle("second-desktop")
    s.check(second.differs(empty, box, step=4) < 0.02,
            "on the second desktop the window isn't shown")
    s.inject("key", "ctrl+alt+Left")
    s.wait_window(title="Control Panel", state="activated")
    back = s.settle("first-desktop")
    s.check(back.differs(shown, box, step=4) < 0.02, "back on the first, the window shows again")

    s.inject("key", "ctrl+alt+shift+Right")
    s.wait(lambda: s.window(title="Control Panel") is not None, "the carried window")
    carried = s.settle("carried")
    s.check(carried.differs(shown, box, step=4) < 0.02, "the carried window still shows")
    s.inject("key", "ctrl+alt+Left")
    s.wait_window(title="Control Panel", gone=True)
    left = s.settle("left-behind")
    s.check(left.differs(empty, box, step=4) < 0.02, "it stayed on the second desktop")

    # The pager: buttons 1 and 2 left of the notification area (in the
    # Classic style, its clock "12:00 PM"); the second shows the window
    # again.
    s.inject("move", WIDTH - 100, HEIGHT - 13, "click", "left")
    s.wait_window(title="Control Panel", state="activated")
    paged = s.settle("paged")
    s.check(paged.differs(carried, box, step=4) < 0.02, "the pager's 2 shows it again")


@scenario(setup=effects_setup(TWO_DESKTOPS))
def virtual_desktops_window_menu(s):
    """With two desktops the window menu (Alt+Space) has Always on Visible
    Workspace, which keeps the window on every desktop, and Move to
    Another Workspace, whose Workspace 2 sends it to the second desktop."""
    empty, box = settle_and_open_control_panel(s)
    shown = s.settle("shown")
    s.inject("key", "alt+space")
    menu = s.settle("window-menu")
    s.check(menu.differs(shown, (box[0], box[1] + 20, 260, 260), step=2) > 0.05,
            "the window menu opened")
    s.inject("key", "v")
    s.settle("sticky")
    s.inject("key", "ctrl+alt+Right")
    time.sleep(0.5)
    s.check(s.window(title="Control Panel") is not None,
            "a window on every desktop keeps its taskbar button on the second")
    second = s.settle("second-desktop")
    s.check(second.differs(shown, box, step=4) < 0.02, "and shows there")
    s.inject("key", "ctrl+alt+Left")
    s.settle("first-again")
    # Not sticky any more, then to Workspace 2 (the submenu's second item).
    s.inject("key", "alt+space", "sleep", 300, "key", "v")
    s.settle("not-sticky")
    s.inject("key", "alt+space", "sleep", 300, "key", "a", "sleep", 300)
    s.settle("submenu")
    s.inject("key", "Down", "key", "Return")
    s.wait_window(title="Control Panel", gone=True)
    moved = s.settle("moved")
    s.check(moved.differs(empty, box, step=4) < 0.02, "Workspace 2 took it off this desktop")
    s.inject("key", "ctrl+alt+Right")
    s.wait_window(title="Control Panel")


@scenario(setup=effects_setup(TWO_DESKTOPS + "[DesktopWall]\nEnabled=true\nSlideDuration=3\n"))
def desktop_wall_slide(s):
    """Desktop Wall: mid-slide to the second desktop the window opened on
    the first has gone part way out to the left, the desktop showing where
    its right side was, and the map of the desktops shows in the middle;
    when the slide is over the window is gone."""
    empty, box = settle_and_open_control_panel(s)
    x, y, w, h = box
    s.inject("key", "ctrl+alt+Right")
    time.sleep(1.4)
    mid = s.shot("mid-slide")
    s.check(mid.differs(empty, (x + 10, y + 60, 120, 200), step=4) > 0.3,
            "mid-slide the window still shows at the left")
    s.check(mid.differs(empty, (x + w - 230, y + 60, 200, 200), step=4) < 0.05,
            "its right side has slid away")
    s.check(mid.differs(empty, (WIDTH // 2 - 30, HEIGHT // 2 - 15, 60, 30), step=2) > 0.2,
            "the map of the desktops shows in the middle")
    s.wait_window(title="Control Panel", gone=True)
    time.sleep(3)
    done = s.settle("slid")
    s.check(done.differs(empty, box, step=4) < 0.02, "the slide over, the second desktop shows")


@scenario(setup=effects_setup(TWO_DESKTOPS + "[DesktopWall]\nEnabled=true\nEdgeFlipMove=true\n"
                              "EdgeFlipDelay=300\n"))
def desktop_wall_edge_flip(s):
    """Desktop Wall's edge flip move: a window dragged by its title bar
    against the right edge of the screen and held there goes to the second
    desktop with the view, coming in from the left; going back to the
    first leaves it behind."""
    empty, box = settle_and_open_control_panel(s)
    x, y, w, h = box
    steps = ["move", x + 100, y + 10, "down", "sleep", 100]
    for i in range(1, 11):
        steps += ["move", x + 100 + (WIDTH - x - 100) * i // 10, y + 10]
    steps += ["move", WIDTH - 1, y + 10, "sleep", 1500, "up"]
    s.inject(*steps)
    flipped = s.settle("flipped")
    s.check(flipped.differs(empty, (0, y + 60, 60, 100), step=4) > 0.3,
            "the window came in at the left of the second desktop")
    s.check(s.window(title="Control Panel") is not None, "it's on the desktop showing")
    s.inject("key", "ctrl+alt+Left")
    s.wait_window(title="Control Panel", gone=True)


@scenario(setup=effects_setup(TWO_DESKTOPS + "[DesktopCube]\nEnabled=true\nRotationTime=3\n"),
          fx=True)
def desktop_cube_turn(s):
    """Desktop Cube (xd-compositor-fx): Ctrl+Alt+Right turns the cube of
    the two desktops; mid-turn the screen is neither desktop flat (both
    in perspective, zoomed out, the desktop colour round them); then the
    second desktop shows, without the window."""
    require_opengl_effects(s)
    empty, box = settle_and_open_control_panel(s)
    shown = s.settle("shown")
    s.inject("key", "ctrl+alt+Right")
    s.wait_window(title="Control Panel", gone=True)
    time.sleep(1.2)
    mid = s.shot("mid-turn")
    whole = (0, 0, WIDTH, HEIGHT)
    s.check(mid.differs(shown, whole, step=8) > 0.3, "mid-turn it isn't the first desktop flat")
    s.check(mid.differs(empty, (0, HEIGHT - 30, WIDTH, 30), step=4) > 0.3,
            "nor the second: the taskbars are turned with their desktops")
    time.sleep(3)
    done = s.settle("turned")
    s.check(done.differs(empty, whole, step=8) < 0.02, "turned, the second desktop shows")


@scenario(setup=effects_setup(TWO_DESKTOPS + "[DesktopCube]\nEnabled=true\n"
                              "[RotateCube]\nEnabled=true\n"), fx=True)
def rotate_cube_drag(s):
    """Rotate Cube (xd-compositor-fx): dragging with Ctrl+Alt and the left
    button turns the cube; held mid-drag the screen is the cube, zoomed
    out, not the desktop; let go past half-way, it settles on the second
    desktop."""
    require_opengl_effects(s)
    empty, box = settle_and_open_control_panel(s)
    shown = s.settle("shown")
    steps = ["hold", "ctrl", "hold", "alt", "move", 900, 300, "down"]
    for i in range(1, 9):
        steps += ["move", 900 - 75 * i, 300]
    drag = s.launch(s.tool("xd-inject"), *map(str, steps + ["sleep", 2500, "up", "release", "alt",
                                                            "release", "ctrl"]))
    time.sleep(1.2)
    mid = s.shot("mid-drag")
    whole = (0, 0, WIDTH, HEIGHT)
    s.check(mid.differs(shown, whole, step=8) > 0.3, "mid-drag the cube shows, not the desktop")
    s.check(mid.differs(shown, (0, 0, WIDTH, 20), step=4) > 0.3,
            "zoomed out: the desktop colour above the cube")
    drag.wait(timeout=30)
    s.wait_window(title="Control Panel", gone=True)
    time.sleep(1)
    done = s.settle("settled")
    s.check(done.differs(empty, whole, step=8) < 0.02, "it settled on the second desktop")


@scenario(setup=effects_setup(TWO_DESKTOPS + "[Expo]\nEnabled=true\n"), fx=True)
def expo_wall(s):
    """Expo (xd-compositor-fx): Win+W shows the two desktops side by side,
    the window small on the first, a dark gap between them; a window
    dragged from the first desktop to the second goes there; a click on
    the second desktop shows it, with the window."""
    require_opengl_effects(s)
    empty, box = settle_and_open_control_panel(s)
    s.inject("key", "super+w")
    wall = s.settle("wall")
    gap = wall.pixel(WIDTH // 2, HEIGHT // 2)
    s.check(max(gap) < 60, f"a dark gap between the desktops ({gap})")
    s.check(wall.differs(empty, (40, 230, 300, 200), step=4) > 0.3,
            "the first desktop has the window, small")
    right = wall.pixel(3 * WIDTH // 4, HEIGHT // 2)
    s.check(right == empty.pixel(WIDTH // 2, HEIGHT // 2) or max(right) > 60,
            f"the second desktop shows beside it ({right})")
    steps = ["move", 200, 330, "down", "sleep", 100]
    for i in range(1, 11):
        steps += ["move", 200 + 56 * i, 330]
    s.inject(*steps, "sleep", 300, "up")
    s.settle("dropped")
    s.inject("move", 3 * WIDTH // 4, HEIGHT - 220, "click", "left")
    s.wait_window(title="Control Panel")
    there = s.settle("second-desktop")
    s.check(there.differs(empty, (0, 0, WIDTH, HEIGHT - 40), step=8) > 0.1,
            "the second desktop shows, with the window dropped on it")
    s.inject("key", "ctrl+alt+Left")
    s.wait_window(title="Control Panel", gone=True)


def count_reddish(shot, box, step=2):
    """How many points of box are clearly red (an annotation, even shrunk
    and blended)."""
    x0, y0, w, h = box
    return sum(1 for y in range(y0, y0 + h, step) for x in range(x0, x0 + w, step)
               if (lambda p: p[0] > 140 and p[0] - p[1] > 70 and p[0] - p[2] > 70)(
                   shot.pixel(x, y)))


@scenario(setup=effects_setup(TWO_DESKTOPS + "[Expo]\nEnabled=true\n[Annotate]\nEnabled=true\n"),
          fx=True)
def expo_leaves_out_effects(s):
    """Expo (xd-compositor-fx) with Annotate on: a red line drawn over the
    desktop is an effect's picture, not the desktop's, so Win+W's pictures
    of the desktops leave it out; Win+W again, and it's back over the
    desktop."""
    require_opengl_effects(s)
    s.inject("move", WIDTH - 20, 300)
    s.settle("desktop")
    steps = ["hold", "super", "hold", "alt", "move", 150, 200, "down"]
    for i in range(1, 11):
        steps += ["move", 150 + 50 * i, 200 + 20 * i]
    steps += ["up", "release", "alt", "release", "super", "move", WIDTH - 20, 300]
    s.inject(*steps)
    drawn = s.settle("drawn")
    # Above the desktops' pictures' taskbars (their icons have red).
    whole = (0, 0, WIDTH, 540)
    before = count_reddish(drawn, whole)
    s.check(before > 50, f"the red line is drawn ({before} red points)")
    s.inject("key", "super+w")
    wall = s.settle("wall")
    s.check(max(wall.pixel(WIDTH // 2, HEIGHT // 2)) < 60, "Expo shows the desktops")
    inside = count_reddish(wall, whole)
    s.check(inside == 0, f"no red line in the desktops' pictures ({inside} red points)")
    s.inject("key", "super+w")
    after = count_reddish(s.settle("back"), whole)
    s.check(after > 50, f"Expo gone, the line is over the desktop again ({after})")


@scenario(setup=effects_setup("[ShiftSwitcher]\nEnabled=true\n"), fx=True)
def shift_switcher_cover(s):
    """Shift Switcher (xd-compositor-fx): with two windows open, Win+Shift+Tab
    held shows them as pictures in a cover flow over the dimmed desktop;
    let go, the one before (Control Panel) is activated, as Alt+Tab
    would."""
    require_opengl_effects(s)
    empty, box = settle_and_open_control_panel(s)
    s.launch("xd-files", "computer:///")
    s.wait_window(title="My Computer", state="activated")
    both = s.settle("both")
    held = s.launch(s.tool("xd-inject"), "hold", "super", "hold", "shift", "key", "Tab", "sleep",
                    "2500", "release", "shift", "release", "super")
    time.sleep(1.2)
    cover = s.shot("cover")
    corner = cover.pixel(WIDTH - 20, HEIGHT // 2)
    desktop = empty.pixel(WIDTH - 20, HEIGHT // 2)
    s.check(sum(corner) < sum(desktop) * 0.7, f"the desktop is dimmed ({corner} < {desktop})")
    s.check(cover.differs(both, (0, 0, WIDTH, HEIGHT), step=8) > 0.3,
            "the windows show as pictures, not as they were")
    held.wait(timeout=30)
    s.wait_window(title="Control Panel", state="activated")


@scenario(AERO_INI)
def start7_search_caret(s):
    """Windows 7's Start menu opens with the focus in its search box: the
    box has Windows 7's blue focused border and its caret blinks."""
    s.inject("key", "ctrl+Escape")
    menu = s.settle("start-menu")
    focused = (0x3D, 0x7B, 0xAD)  # Edit's focused border, measured
    area = (0, HEIGHT - 120, 360, 80)  # the menu's bottom left, over the taskbar
    xs, ys = [], []
    for y in range(area[1], area[1] + area[3]):
        for x in range(area[0], area[0] + area[2]):
            if max(abs(p - q) for p, q in zip(menu.pixel(x, y), focused)) <= 4:
                xs.append(x)
                ys.append(y)
    # Its top edge: the sides and bottom are lighter, as in Windows 7.
    s.check(len(xs) > 200, f"the search box has the focused border ({len(xs)} px)")
    box = (min(xs) + 2, min(ys) + 2, max(xs) - min(xs) - 3, 18)
    # Over three blink times the box changes (the caret) and nothing
    # else in the menu does.
    shots = [s.shot("caret")]
    for _ in range(6):
        time.sleep(0.27)
        shots.append(s.shot("caret"))
    changes = [b.changed_box(a, box, step=1, tolerance=40) for a, b in zip(shots, shots[1:])]
    changes = [c for c in changes if c is not None]
    s.check(changes != [], "the caret blinks")
    s.check(all(c[2] <= 3 for c in changes), f"only the caret changes ({changes})")
    s.inject("type", "cal")
    time.sleep(0.1)
    typed = s.shot("typed")
    s.check(typed.differs(menu, box, step=1) > 0, "typing shows in the box")


def taskbar_top(shot):
    """The taskbar's top row: the first, going down the middle from above
    where the tallest one starts, that isn't the desktop's colour."""
    desktop = shot.pixel(WIDTH // 2, HEIGHT - 60)
    for y in range(HEIGHT - 60, HEIGHT):
        if max(abs(p - q) for p, q in zip(shot.pixel(WIDTH // 2, y), desktop)) > 8:
            return y
    return HEIGHT


@scenario(LUNA_INI + "\n[Taskbar]\nLayout=unified\nButtons=always\n")
def unified_taskbar(s):
    """Luna with the unified taskbar: Windows 7's large icons make it 40px
    tall; "Always combine, hide labels" gives Files' two windows one icon
    button, as wide as 7's; "Use small icons" makes it XP's 30px again."""
    empty = s.settle("taskbar")
    s.check(taskbar_top(empty) == HEIGHT - 40, f"40px tall ({HEIGHT - taskbar_top(empty)})")
    for folder in ("Documents", "Pictures"):
        s.launch("xd-files", os.path.join(s.home, folder))
        s.wait_window(title=folder, state="activated")
    s.inject("move", WIDTH // 2, HEIGHT // 2)
    after = s.settle("two-windows")
    # Left of the clock, which may turn over meanwhile.
    button = after.changed_box(empty, (0, HEIGHT - 40, WIDTH - 160, 40))
    s.check(button is not None and 40 <= button[2] <= 64,
            f"one icon button for both windows ({button})")  # fmt: skip
    path = os.path.join(s.config, "desktop.ini")
    with open(path) as f:
        text = f.read()
    with open(path + ".new", "w") as f:
        f.write(text.replace("[Taskbar]\n", "[Taskbar]\nIcons=small\n", 1))
    os.replace(path + ".new", path)
    s.wait(lambda: taskbar_top(s.shot("small")) == HEIGHT - 30, "the taskbar to be 30px tall")
    s.log("ok: small icons, 30px")


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


# A program sending one notification (org.freedesktop.Notifications'
# Notify): argv log, summary, body, expire_timeout, then action key and
# label pairs. It writes "id N", then the signals it hears of it, a line
# each ("ActionInvoked N key", "NotificationClosed N reason"), and ends
# once it's closed; with "--once" for the actions, at once (notify-send).
NOTIFY_CLIENT = r"""
import sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

log, summary, body, expire = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
actions = sys.argv[5:]
once = actions == ["--once"]
if once:
    actions = []
bus = Gio.bus_get_sync(Gio.BusType.SESSION)
loop = GLib.MainLoop()
mine = {}

def write(line):
    with open(log, "a") as f:
        f.write(line + "\n")

def on_signal(conn, sender, path, iface, name, params):
    values = params.unpack()
    if values[0] != mine.get("id"):
        return
    write(name + " " + " ".join(str(v) for v in values))
    if name == "NotificationClosed":
        loop.quit()

for name in ("ActionInvoked", "NotificationClosed"):
    bus.signal_subscribe(None, "org.freedesktop.Notifications", name,
                         "/org/freedesktop/Notifications", None, 0, on_signal)
reply = bus.call_sync("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                      "org.freedesktop.Notifications", "Notify",
                      GLib.Variant("(susssasa{sv}i)", ("Backup", 0, "dialog-information",
                                   summary, body, actions,
                                   {"desktop-entry": GLib.Variant("s", "org.xeondesktop.Files")},
                                   expire)),
                      GLib.VariantType("(u)"), 0, 5000, None)
mine["id"] = reply.unpack()[0]
write("id " + str(mine["id"]))
if once:
    sys.exit(0)
GLib.timeout_add_seconds(60, loop.quit)
loop.run()
"""


def notified(log):
    """The notification client's log lines."""
    return open(log).read().split("\n")[:-1] if os.path.exists(log) else []


@scenario(LUNA_INI)
def notification_balloons(s):
    """Programs' notifications as XP's balloons from the notification
    area: a click on one tells its program (its default action) and puts
    it away; one that times out tells its program it expired and stays in
    the history, which the notifications' icon opens; with Do Not Disturb
    on, they go straight to the history, unseen."""
    before = s.settle("desktop")
    above_tray = (WIDTH - 420, HEIGHT - 330, 420, 300)
    first = os.path.join(s.base, "first")
    s.launch(sys.executable, "-c", NOTIFY_CLIENT, first, "Backup finished",
             "All your files were copied to the <b>backup</b> drive.", "-1", "default", "Open")
    shot, box = wait_change(s, before, above_tray, "the notification's balloon", "balloon")
    s.check(box is not None and box[0] + box[2] > WIDTH - 60 and box[1] + box[3] >= HEIGHT - 40,
            f"a balloon pointing into the notification area ({box})")  # fmt: skip
    s.log("ok: the notification showed as a balloon")
    # The stem's tip, over the notifications' icon.
    icon_x, icon_y = box[0] + box[2] - 16, HEIGHT - 14
    s.inject("move", box[0] + box[2] // 2, box[1] + 36, "sleep", 300, "click")
    s.wait(lambda: notified(first)[1:] == ["ActionInvoked 1 default", "NotificationClosed 1 2"],
           "the click to reach the program (default action, then closed)")  # fmt: skip
    s.wait(lambda: s.shot("gone").differs(before, above_tray) < 0.01, "the balloon to go")
    s.log("ok: a click told the program and put the balloon away")

    second = os.path.join(s.base, "second")
    s.launch(sys.executable, "-c", NOTIFY_CLIENT, second, "Download complete",
             "report.pdf", "1000")
    wait_change(s, before, above_tray, "the second balloon", "second")
    s.inject("move", WIDTH // 2, HEIGHT // 2)
    s.wait(lambda: notified(second)[1:] == ["NotificationClosed 2 1"],
           "the program to hear it expired", timeout=15)  # fmt: skip
    s.wait(lambda: s.shot("expired").differs(before, (WIDTH - 420, HEIGHT - 330, 420, 290)) < 0.01,
           "the second balloon to go")  # fmt: skip
    s.log("ok: a balloon timed out as its program asked")

    closed = s.settle("closed")
    s.inject("move", icon_x, icon_y, "click")
    history_area = (WIDTH - 420, HEIGHT - 480, 420, 450)
    history, panel = wait_change(s, closed, history_area, "the history", "history")
    s.check(panel is not None and panel[3] >= 400, f"the history opened ({panel})")
    s.log("ok: the notifications' icon opened the history")
    s.inject("key", "Escape")
    s.wait(lambda: s.shot("history-closed").differs(closed, history_area) < 0.01,
           "Esc to close the history")  # fmt: skip

    with open(os.path.join(s.config, "desktop.ini"), "a") as f:
        f.write("\n[Taskbar]\nDoNotDisturb=true\n")
    time.sleep(1.5)
    quiet = s.settle("quiet")
    third = os.path.join(s.base, "third")
    s.launch(sys.executable, "-c", NOTIFY_CLIENT, third, "Quiet one", "Not now", "-1")
    s.wait(lambda: notified(third)[:1] == ["id 3"], "the third notification to be taken")
    time.sleep(2)
    s.check(s.shot("unseen").differs(quiet, above_tray) < 0.01,
            "Do Not Disturb: no balloon")  # fmt: skip
    kept = os.path.join(os.environ["XDG_STATE_HOME"], "xeondesktop", "notifications.ini")

    def kept_unread():
        if not os.path.exists(kept):
            return False
        c = configparser.ConfigParser(interpolation=None)
        c.read(kept)
        return any(c[g].get("Summary") == "Quiet one" and c[g].get("Unread") == "true"
                   for g in c.sections())

    s.wait(kept_unread, "the quiet one in the history, unread")
    s.log("ok: with Do Not Disturb on, it went to the history unseen")

    # Do Not Disturb off; a program that's gone by the time it shows.
    with open(os.path.join(s.config, "desktop.ini"), "w") as f:
        f.write(LUNA_INI)
    time.sleep(1.5)
    loud = s.settle("loud")
    fourth = os.path.join(s.base, "fourth")
    s.launch(sys.executable, "-c", NOTIFY_CLIENT, fourth, "Sent and gone", "Like notify-send",
             "-1", "--once")
    wait_change(s, loud, above_tray, "the balloon of a program that's gone", "gone-program")
    s.log("ok: a program that left at once still got its balloon")


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


# A program sending Unity's LauncherEntry updates for Explorer: each line
# on stdin is the a{sv} of one Update (GVariant text); it answers "sent".
LAUNCHER_SENDER = r"""
import sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

bus = Gio.bus_get_sync(Gio.BusType.SESSION)
for line in sys.stdin:
    props = GLib.Variant.parse(GLib.VariantType("a{sv}"), line.strip(), None, None)
    bus.emit_signal(None, "/com/canonical/unity/launcherentry/xdtest",
                    "com.canonical.Unity.LauncherEntry", "Update",
                    GLib.Variant.new_tuple(GLib.Variant("s", "application://org.xeondesktop.Files.desktop"),
                                           props))
    bus.flush_sync(None)
    print("sent", flush=True)
"""


def launcher_sender(s):
    """Starts LAUNCHER_SENDER; returns a function sending one Update."""
    p = subprocess.Popen([sys.executable, "-c", LAUNCHER_SENDER], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True, preexec_fn=die_with_parent)
    s.children.append(p)

    def send(props):
        p.stdin.write(props + "\n")
        p.stdin.flush()
        if p.stdout.readline().strip() != "sent":
            raise Failure("the LauncherEntry sender died")

    return p, send


def green_share(shot, rect):
    """The share of rect's pixels that are clearly green (the progress)."""
    x0, y0, w, h = rect
    total = hits = 0
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            r, g, b = shot.pixel(x, y)
            total += 1
            if g > r + 30 and g > b + 15:
                hits += 1
    return hits / max(total, 1)


@scenario(AERO_INI)
def superbar_launcher_entry(s):
    """Aero: a program's LauncherEntry progress fills its button green (none
    at 0%, half at 50%, all at 100%) and goes when hidden, when the sender
    leaves the bus and when its last window closes; its count is a badge
    over the icon; urgent flashes the button."""
    cx, cy = files_button(s)
    lit = s.settle("lit")
    bar = (0, HEIGHT - 40, WIDTH, 40)
    # The button: Windows 7's 58px wide one, centred on cx.
    x0, x1 = cx - 27, cx + 27
    # Strips either side of the icon, clear of the corners.
    left = (x0 + 3, HEIGHT - 30, 7, 20)
    right = (x1 - 10, HEIGHT - 30, 7, 20)
    sender, send = launcher_sender(s)

    send("{'progress': <0.0>, 'progress-visible': <true>}")
    zero = s.settle("progress-0")
    s.check(green_share(zero, (x0, HEIGHT - 38, x1 - x0, 36)) < 0.02,
            "no progress shows at 0%")  # fmt: skip
    send("{'progress': <0.5>}")
    half = s.settle("progress-50")
    gl, gr = green_share(half, left), green_share(half, right)
    s.check(gl > 0.8 and gr < 0.1, f"50% fills the left half green ({gl:.2f}, {gr:.2f})")
    s.log("ok: 50% progress fills half the button")
    send("{'progress': <1.0>}")
    full = s.settle("progress-100")
    gl, gr = green_share(full, left), green_share(full, right)
    s.check(gl > 0.8 and gr > 0.8, f"100% fills the button green ({gl:.2f}, {gr:.2f})")
    s.log("ok: 100% progress fills the button")
    send("{'progress-visible': <false>}")
    hidden = s.settle("progress-hidden")
    s.check(hidden.differs(lit, bar, step=1) == 0, "the progress goes when hidden")
    s.log("ok: progress-visible false hides it")

    # The count: a red badge at the icon's bottom right corner.
    send("{'count': <int64 3>, 'count-visible': <true>}")
    badge = s.settle("count")
    corner = (cx, HEIGHT - 20, 16, 16)
    reds = share_like(badge, corner, (0xD0, 0x30, 0x2C), step=1, tolerance=40)
    s.check(reds > 0.2 and badge.differs(lit, corner, step=1) > 0.3,
            f"the count is a red badge over the icon ({reds:.2f})")  # fmt: skip
    s.check(badge.differs(lit, (x0, HEIGHT - 38, cx - x0, 36), step=1) == 0,
            "the badge is only at the corner")  # fmt: skip
    s.log("ok: the count shows as a badge")
    send("{'count-visible': <false>}")
    s.check(s.settle("count-hidden").differs(lit, bar, step=1) == 0, "the badge goes when hidden")

    # Urgent, with another window active: the button flashes, then stays
    # lit until urgent ends.
    s.launch("xd-run")
    s.wait_window(title="Run", state="activated")
    s.inject("move", WIDTH // 2, HEIGHT // 2)
    calm = s.settle("other-active")
    send("{'urgent': <true>}")
    looks = set()
    for i in range(8):
        shot = s.shot(f"urgent-{i}")
        looks.add(shot.differs(calm, (x0, HEIGHT - 38, x1 - x0, 36), step=2) > 0.2)
        time.sleep(0.25)
    s.check(looks == {True, False}, f"urgent flashes the button ({looks})")
    s.log("ok: urgent flashes the button")
    send("{'urgent': <false>}")
    s.check(s.settle("not-urgent").differs(calm, bar, step=1) == 0,
            "the button stops flashing when urgent ends")  # fmt: skip
    s.log("ok: urgent false stops it")

    # The sender leaving the bus takes its progress with it.
    send("{'progress': <0.5>, 'progress-visible': <true>}")
    s.check(green_share(s.settle("progress-again"), left) > 0.8, "progress shows again")
    sender.stdin.close()
    sender.wait(timeout=10)
    gone = s.settle("sender-gone")
    s.check(gone.differs(calm, bar, step=1) == 0, "the progress goes with its sender")
    s.log("ok: the sender's leaving clears it")

    # And the last window's closing (Files stays pinned).
    sender, send = launcher_sender(s)
    send("{'progress': <0.5>, 'progress-visible': <true>}")
    s.check(green_share(s.settle("progress-third"), left) > 0.8, "progress shows a third time")
    s.inject("key", "Escape")
    s.wait_window(title="Run", gone=True)
    s.wait_window(title="Documents", state="activated")
    s.inject("key", "alt+F4")
    s.wait_window(title="Documents", gone=True)
    closed = s.settle("closed")
    s.check(green_share(closed, (x0, HEIGHT - 38, x1 - x0, 36)) < 0.02,
            "the progress goes when the last window closes")  # fmt: skip
    s.log("ok: closing the last window clears it")
    sender.stdin.close()
    sender.wait(timeout=10)


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


def open_documents(s, explore=False):
    """xd-files on Documents (Letters, then Notes.txt), the pointer out of
    the way, explored (with the Folders tree) if asked; returns the
    window's top left corner and its screenshot."""
    empty = s.settle("desktop")
    s.launch("xd-files", *(["--explore"] if explore else []), os.path.join(s.home, "Documents"))
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
    x, y, one = open_documents(s, explore=True)  # the tree's Home below
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


def write_png(path, w, h, rgb):
    """A plain w x h PNG of one colour."""
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    os.makedirs(os.path.dirname(path), exist_ok=True)
    pb = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, w, h)
    pb.fill((rgb << 8) | 0xFF)
    pb.savev(path, "png", [], [])


# The Folders button, from the window's top left corner (Classic).
FOLDERS_BUTTON = (246, 72)


def share_like(shot, rect, rgb, step=2, tolerance=6):
    """The share (0..1) of sampled pixels in rect that are rgb."""
    x0, y0, w, h = rect
    total = hits = 0
    for y in range(y0, y0 + h, step):
        for x in range(x0, x0 + w, step):
            total += 1
            if max(abs(p - q) for p, q in zip(shot.pixel(x, y), rgb)) <= tolerance:
                hits += 1
    return hits / max(total, 1)


@scenario()
def files_task_pane(s):
    """XP's task pane in the Folders bar's place, drawn from a theme
    package's shell/NAME.xml (one made here, with plain colours): the
    pane's background, the groups' title bars (an image) and their Expand
    and Collapse buttons, and the groups' bodies; a title bar folds its
    group away."""
    pkg = os.path.join(s.home, "Test.xdtheme.d")
    os.makedirs(os.path.join(pkg, "styles"), exist_ok=True)
    os.makedirs(os.path.join(pkg, "shell"), exist_ok=True)
    with open(os.path.join(pkg, "theme.xml"), "w") as f:
        f.write('<xdtheme version="1" name="Test" base="classic">'
                '<variant id="plain" name="Plain" size="Normal" style="styles/plain.xml"'
                ' shell="shell/plain.xml"/></xdtheme>')
    with open(os.path.join(pkg, "styles", "plain.xml"), "w") as f:
        f.write("<style/>")
    pane, head, arrow, body = (0xC0, 0x80, 0x40), 0x2040A0, 0x40A020, (0xE0, 0xF0, 0xD0)
    write_png(os.path.join(pkg, "images", "head.png"), 40, 21, head)
    write_png(os.path.join(pkg, "images", "arrow.png"), 21, 23, arrow)
    with open(os.path.join(pkg, "shell", "plain.xml"), "w") as f:
        f.write('<shell><class name="ExplorerBar">'
                '<part BgType="BorderFill" FillType="Solid" FillColor="%d %d %d"'
                ' ContentMargins="12 12 12 12" GroupSpacing="15"/>'
                '<part name="NormalGroupHead" BgType="ImageFile" ImageFile="images/head.png"'
                ' SizingType="Stretch" ContentMargins="12 3 2 0" Font="Tahoma, 8, Bold"'
                ' TextColor="255 255 255"/>'
                '<part name="NormalGroupExpand" BgType="ImageFile" ImageFile="images/arrow.png"'
                ' ImageCount="1" SizingType="TrueSize"/>'
                '<part name="NormalGroupCollapse" BgType="ImageFile" ImageFile="images/arrow.png"'
                ' ImageCount="1" SizingType="TrueSize"/>'
                '<part name="NormalGroupBackground" BgType="BorderFill" FillType="Solid"'
                ' FillColor="%d %d %d" ContentMargins="13 13 10 11" TextColor="0 0 255"'
                ' LinkSpacing="4" IconSpacing="6"/>'
                '</class></shell>' % (pane + body))
    empty = s.settle("desktop")
    # Explored: the Folders tree first, the task pane with Folders off.
    s.launch("xd-files", "--explore", os.path.join(s.home, "Documents"),
             env={"XD_THEME_PACKAGE": pkg})
    s.wait_window(app_id=FILES, state="activated")
    files_title(s, "Documents", "Documents to open")
    s.inject("move", WIDTH - 24, HEIGHT - 80)
    shot = s.settle("documents")
    x, y, _, _ = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    left = (x + 8, y + 140, 190, 300)  # where the Folders bar is
    s.check(share_like(shot, left, pane) < 0.05, "the Folders bar shows at first")
    s.inject("move", x + FOLDERS_BUTTON[0], y + FOLDERS_BUTTON[1], "click", "sleep", 300,
             "move", WIDTH - 24, HEIGHT - 80)
    tasks = s.settle("task pane")
    s.check(share_like(tasks, left, pane) > 0.2, "Folders off: the task pane, its background the package's")
    s.check(share_like(tasks, left, (0x20, 0x40, 0xA0)) > 0.02, "the groups' title bars, the package's image")
    s.check(share_like(tasks, left, (0x40, 0xA0, 0x20)) > 0.005, "their Expand and Collapse buttons")
    s.check(share_like(tasks, left, body) > 0.05, "the groups' bodies, the package's colour")
    # Folding File and Folder Tasks: its body goes, the pane shows instead.
    before = share_like(tasks, left, body)
    bx, by, bw, bh = left
    # The rows of the first title bar in the rect, clicked in their middle:
    # the rect may start partway down one.
    title_rows = []
    for row in range(by, by + bh):
        if share_like(tasks, (bx, row, bw, 1), (0x20, 0x40, 0xA0), step=1) > 0.5:
            title_rows.append(row)
        elif title_rows:
            break
    s.check(len(title_rows) > 0, "the first group's title bar is found")
    title = (title_rows[0] + title_rows[-1]) // 2
    s.inject("move", bx + 60, title, "click", "sleep", 300, "move", WIDTH - 24, HEIGHT - 80)
    folded = s.settle("folded")
    s.check(share_like(folded, left, body) < before - 0.01, "a click on the title bar folds the group")


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
def files_wheel_scrolls(s):
    """The mouse wheel over the view scrolls a folder longer than the
    window, and back."""
    many = os.path.join(s.home, "Documents", "Many")
    os.makedirs(many)
    for i in range(300):
        open(os.path.join(many, f"file {i:03}.txt"), "w").close()
    empty = s.settle("desktop")
    s.launch("xd-files", many)
    s.wait_window(app_id=FILES, state="activated")
    files_title(s, "Many", "Many to open")
    s.inject("move", WIDTH - 24, HEIGHT - 80)
    shown = s.settle("the folder")
    x, y, _, _ = shown.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.inject("move", x + 400, y + 250)
    top = s.settle("the folder's top")
    view = (x + 200, y + 120, 400, 250)
    s.inject("wheel", 5)
    down = s.settle("wheeled down")
    s.check(down.differs(top, view) > 0.02, "the wheel scrolled the view down")
    s.inject("wheel", -5)
    up = s.settle("wheeled up")
    s.check(up.differs(top, view) < 0.01, "and back up to the top")


@scenario()
def files_big_folder_fills_in(s):
    """A folder of 40,000 files shows its first ones while the rest are
    still being listed, as Windows fills a big folder in: the status
    bar's count goes on growing after the first items show."""
    big = os.path.join(s.home, "Documents", "Big")
    os.makedirs(big)
    for i in range(40000):
        open(os.path.join(big, f"file {i:05}.txt"), "w").close()
    x, y, _ = open_documents(s)
    view = (x + 240, y + 130, 560, 420)
    status = (x + 4, y + 566, 200, 16)
    blank = s.shot("blank")
    s.inject("key", "ctrl+l", "type", big, "key", "Return")
    # A blank page has one colour; items have icons and names.
    first = s.wait(lambda: (lambda shot: shot if shot.differs(blank, view) > 0.02 else None)(
        s.shot("filling")), "the first items to show", timeout=60)
    files_title(s, "Big", "Big to open")
    done = s.settle("all listed", timeout=60)
    s.check(done.differs(first, status) > 0.02,
            "the count grew after the first items showed")  # fmt: skip


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


def search_tree(s):
    """A tree to search under Documents/Finds: names, contents, a big
    file, an old one, a picture and a hidden folder."""
    root = os.path.join(s.home, "Documents", "Finds")
    files = {
        "report.txt": "the quarterly figures",
        "Report 2025.odt": "draft",
        "notes.md": "remember the milk",
        "sub/deeper/report-old.txt": "older figures",
        ".hidden/secret report.txt": "hidden",
        "old.txt": "from long ago",
    }
    for name, text in files.items():
        path = os.path.join(root, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(text)
    with open(os.path.join(root, "big.bin"), "wb") as f:
        f.write(b"\0" * (2 * 1024 * 1024))
    write_png(os.path.join(root, "sub", "photo.png"), 8, 8, 0x3366CC)
    when = time.mktime((2020, 6, 1, 12, 0, 0, 0, 0, -1))
    os.utime(os.path.join(root, "old.txt"), (when, when))
    return root


def search_trace(s):
    return os.path.join(s.base, "search.log")


def read_trace(s):
    try:
        with open(search_trace(s)) as f:
            return f.read().splitlines()
    except FileNotFoundError:
        return []


def last_search(s, root):
    """The last search's results (relative to root, sorted) once it's done,
    or None while it runs."""
    lines = read_trace(s)
    starts = [i for i, line in enumerate(lines) if line.startswith("start ")]
    if not starts:
        return None
    block = lines[starts[-1] + 1:]
    if not any(line.startswith("done ") for line in block):
        return None
    found = [line[len("found "):] for line in block if line.startswith("found ")]
    return sorted(os.path.relpath(path, root) for path in found)


def expect_search(s, root, expected, what):
    got = s.wait(lambda: last_search(s, root), what + " to finish", timeout=30)
    s.check(got == sorted(expected), f"{what}: found {got}")


def save_search_file(s, name, **keys):
    """A saved search (the .xdsearch key file) under the scratch home."""
    path = os.path.join(s.home, name + ".xdsearch")
    with open(path, "w") as f:
        f.write("[Search]\nVersion=1\n")
        for key, value in keys.items():
            f.write(f"{key}={value}\n")
    return path


def run_saved(s, root, path, expected, what):
    """Opens a saved search (its window runs it), checks what it found and
    closes the window."""
    open(search_trace(s), "w").close()
    s.launch("xd-files", path, env={"XD_SEARCH_TRACE": search_trace(s)})
    s.wait_window(app_id=FILES, title="Search Results", state="activated")
    expect_search(s, root, expected, what)
    s.inject("key", "alt+F4")
    s.wait_window(app_id=FILES, gone=True)


@scenario(desktop_ini=LUNA_INI)
def files_search_companion(s):
    """XP's Search Companion: F3 opens it, All files and folders finds a
    name typed in it (part of the name, ignoring case, hidden files left
    out); saved searches run its other criteria: a wildcard, a regular
    expression, a word in the file, the size, the dates and hidden files;
    the Search bar closes again with F3."""
    root = search_tree(s)
    open(search_trace(s), "w").close()
    s.settle("desktop")
    s.launch("xd-files", root, env={"XD_SEARCH_TRACE": search_trace(s)})
    s.wait_window(app_id=FILES, title="Finds", state="activated")
    s.settle("finds")
    s.inject("key", "F3")
    s.wait(lambda: "search bar open" in read_trace(s), "the Search bar to open")
    s.settle("companion")
    # "What do you want to search for?": the third, All files and folders.
    s.inject("key", "Tab", "key", "Tab", "key", "Return", "sleep", 300)
    s.settle("all files")
    s.inject("type", "REPORT", "key", "Return")
    s.wait_window(app_id=FILES, title="Search Results")
    expect_search(s, root, ["Report 2025.odt", "report.txt", "sub/deeper/report-old.txt"],
                  "a name typed in the Companion")
    s.settle("results")
    s.inject("key", "F3")
    s.wait(lambda: "search bar closed" in read_trace(s), "F3 to close the Search bar")
    s.log("ok: F3 opened and closed the Search Companion")
    s.inject("key", "alt+F4")
    s.wait_window(app_id=FILES, gone=True)
    base = {"LookIn": root}
    run_saved(s, root, save_search_file(s, "wildcard", Name="*.TXT", **base),
              ["old.txt", "report.txt", "sub/deeper/report-old.txt"], "a wildcard")
    run_saved(s, root, save_search_file(s, "regex", Name="^rep.*\\\\.txt$", Regex="true", **base),
              ["report.txt", "sub/deeper/report-old.txt"], "a regular expression")
    run_saved(s, root, save_search_file(s, "contents", Contents="MILK", **base), ["notes.md"],
              "a word in the file")
    run_saved(s, root, save_search_file(s, "size", Size="large", **base), ["big.bin"],
              "Large (more than 1 MB)")
    run_saved(s, root, save_search_file(s, "dates", When="dates", From="2019-01-01",
                                        To="2020-12-31", **base), ["old.txt"],
              "Specify dates")
    run_saved(s, root, save_search_file(s, "hidden", Name="secret", Hidden="true", **base),
              [".hidden/secret report.txt"], "Search hidden files and folders")
    run_saved(s, root, save_search_file(s, "pictures", Kinds="picture;", **base),
              ["sub/photo.png"], "Pictures, music, or video")


@scenario(desktop_ini=AERO_INI)
def files_search_box(s):
    """Windows 7's search box: typing searches the folder and its
    subfolders (the title "Search Results in Finds"); Advanced Query Syntax
    filters, a /regular expression/, and Esc back to the folder."""
    root = search_tree(s)
    open(search_trace(s), "w").close()
    s.settle("desktop")
    s.launch("xd-files", root, env={"XD_SEARCH_TRACE": search_trace(s)})
    s.wait_window(app_id=FILES, title="Finds", state="activated")
    s.settle("finds")
    s.inject("key", "ctrl+e", "sleep", 200, "type", "report -old")
    s.wait_window(app_id=FILES, title="Search Results in Finds")
    expect_search(s, root, ["Report 2025.odt", "report.txt"], "report -old")
    for text, expected, what in [
        ("kind:document size:tiny datemodified:2020", ["old.txt"],
         "kind:, size: and datemodified:"),
        ("/^rep.*\\.TXT$/i", ["report.txt", "sub/deeper/report-old.txt"], "a /regex/i"),
        ("content:/fig.res/ OR size:>1MB", ["big.bin", "report.txt", "sub/deeper/report-old.txt"],
         "content:/regex/ OR size:"),
    ]:
        open(search_trace(s), "w").close()
        s.inject("key", "ctrl+a", "type", text)
        expect_search(s, root, expected, what)
    s.settle("results")
    s.inject("key", "ctrl+a", "type", "/a(/")
    s.wait(lambda: any(line.startswith("error ") and "a(" in line for line in read_trace(s)),
           "an invalid pattern's error")
    s.check(s.window(app_id=FILES) is not None, "an invalid pattern is an error, not a crash")
    s.inject("key", "Escape")
    s.wait_window(app_id=FILES, title="Finds")
    s.log("ok: Esc went back to the folder")


@scenario()
def files_search_stop(s):
    """Stopping a big search (2000's Search pane): Stop Search ends it, what
    it found so far stays, and nothing more comes."""
    big = os.path.join(s.home, "Documents", "Big")
    for i in range(600):
        sub = os.path.join(big, f"dir {i:03}")
        os.makedirs(sub)
        for j in range(50):
            with open(os.path.join(sub, f"file {j:02}.txt"), "w") as f:
                f.write("some words " * 20)
    open(search_trace(s), "w").close()
    s.settle("desktop")
    s.launch("xd-files", "--search", big, env={"XD_SEARCH_TRACE": search_trace(s)})
    s.wait_window(app_id=FILES, title="Big", state="activated")
    s.wait(lambda: "search bar open" in read_trace(s), "the Search pane to open")
    # Searching the contents makes it slow enough to stop.
    s.inject("type", "file", "key", "Tab", "type", "nothing like this", "key", "Return")
    s.wait_window(app_id=FILES, title="Search Results")
    s.inject("key", "alt+p")
    s.wait(lambda: "stopped" in read_trace(s), "Stop Search to stop it")
    count = len(read_trace(s))
    time.sleep(1.5)
    lines = read_trace(s)
    s.check(len(lines) == count and not any(line.startswith("done ") for line in lines),
            "nothing came after Stop Search")


@scenario()
def files_search_streams(s):
    """A big tree's results come in as they're found: the first show while
    the search is still going."""
    big = os.path.join(s.home, "Documents", "Many")
    for i in range(200):
        sub = os.path.join(big, f"dir {i:03}")
        os.makedirs(sub)
        for j in range(100):
            open(os.path.join(sub, f"match {j:02}.txt"), "w").close()
    path = save_search_file(s, "many", Name="match", LookIn=big)
    open(search_trace(s), "w").close()
    s.settle("desktop")
    s.launch("xd-files", path, env={"XD_SEARCH_TRACE": search_trace(s)})
    s.wait_window(app_id=FILES, title="Search Results")

    def partial():
        lines = read_trace(s)
        found = sum(line.startswith("found ") for line in lines)
        return found > 0 and not any(line.startswith("done ") for line in lines)

    s.wait(partial, "results while the search still runs", timeout=60)
    s.log("ok: the first results came while the search was still going")
    s.wait(lambda: any(line.startswith("done ") for line in read_trace(s)), "the search to end",
           timeout=100)
    s.check("done 20000" in read_trace(s), "all 20,000 found in the end")


@scenario(desktop_ini=LUNA_INI)
def files_save_search(s):
    """File > Save Search saves the criteria in ~/Searches; opening the
    saved search runs it again."""
    root = search_tree(s)
    open(search_trace(s), "w").close()
    s.settle("desktop")
    s.launch("xd-files", root, env={"XD_SEARCH_TRACE": search_trace(s)})
    s.wait_window(app_id=FILES, title="Finds", state="activated")
    s.settle("finds")
    s.inject("key", "ctrl+f")
    s.wait(lambda: "search bar open" in read_trace(s), "Ctrl+F to open the Search bar")
    s.settle("companion")
    s.inject("key", "Tab", "key", "Tab", "key", "Return", "sleep", 300)
    s.inject("type", "notes", "key", "Return")
    expect_search(s, root, ["notes.md"], "notes")
    s.settle("results")
    results = s.settle("before saving")
    s.inject("key", "alt+f", "sleep", 300, "key", "h")
    # The dialog (a child of the window, so not in xd-windows' list).
    s.wait(lambda: s.shot("save dialog").differs(results, (0, 0, WIDTH, HEIGHT - 40)) > 0.2,
           "the Save Search dialog")
    s.settle("save dialog")
    s.inject("key", "Return")
    saved = os.path.join(s.home, "Searches", "Files named notes.xdsearch")
    s.wait(lambda: os.path.exists(saved), "the saved search in ~/Searches")
    c = configparser.ConfigParser(interpolation=None)
    c.optionxform = str
    c.read(saved)
    s.check(c["Search"].get("Name") == "notes" and c["Search"].get("LookIn") == root + ";",
            "it holds the name and Look in")
    s.inject("key", "alt+F4")
    s.wait_window(app_id=FILES, gone=True)
    run_saved(s, root, saved, ["notes.md"], "the saved search opened again")


@scenario()
def files_search_highlight(s):
    """The matched part of each result's name is highlighted: by default in
    the theme's selected-text colours (white on Classic's dark blue);
    Folder Options' Search tab colour (SearchHighlight) changes it in the
    open window."""
    root = search_tree(s)
    path = save_search_file(s, "report", Name="report", LookIn=root)
    open(search_trace(s), "w").close()
    empty = s.settle("desktop")
    s.launch("xd-files", path, env={"XD_SEARCH_TRACE": search_trace(s)})
    s.wait_window(app_id=FILES, title="Search Results", state="activated")
    expect_search(s, root, ["Report 2025.odt", "report.txt", "sub/deeper/report-old.txt"],
                  "report")
    s.inject("move", WIDTH - 24, HEIGHT - 80)
    shot = s.settle("highlighted")
    x, y, w, h = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    # The results' Name column, right of the Search pane, below the header.
    names = (x + 250, y + 120, 200, 80)
    hilight = (10, 36, 106)
    marked = count_like(shot, names, hilight, tolerance=12)
    s.check(marked > 40, f"the matches are marked in Hilight's colour ({marked} px)")
    with open(os.path.join(s.config, "desktop.ini"), "w") as f:
        f.write(CLASSIC_INI + "\n[Folders]\nSearchHighlight=#ff0000\n")
    s.wait(lambda: count_like(s.shot("red"), names, (255, 0, 0), tolerance=60) > 20,
           "the chosen colour to show")
    red = s.settle("red")
    s.check(count_like(red, names, hilight, tolerance=12) < marked // 4,
            "the theme's colours gave way to the chosen one")


@scenario(desktop_ini=AERO_INI)
def folder_options_search(s):
    """Folder Options' Search tab (7's): Find partial matches off and a
    colour of one's own for highlighting, saved in [Folders]."""
    empty = s.settle("desktop")
    s.launch("xd-folders")
    s.wait_window(title="Folder Options", state="activated")
    x, y, w, h = s.settle("folder options").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.inject("key", "ctrl+Tab", "sleep", 200, "key", "ctrl+Tab", "sleep", 300)
    s.settle("search tab")
    s.inject("key", "alt+p", "sleep", 200, "key", "alt+e", "sleep", 300)
    s.settle("changed")
    s.inject("move", x + w - 208, y + h - 21, "click")
    s.wait(lambda: s.ini().has_section("Folders")
           and s.ini()["Folders"].get("SearchPartial") == "false"
           and s.ini()["Folders"].get("SearchHighlight") == "#ff8c00",
           "SearchPartial=false and SearchHighlight saved")  # fmt: skip
    s.check(s.ini()["Folders"].get("SearchRegex") == "true",
            "Allow regular expressions stays on, its default")


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
def folder_options_common_tasks(s):
    """Folder Options' "Show common tasks in folders", XP's default: a
    folder window opens with the task pane on the left, the Folders button
    puts the Folders tree in its place and takes it away again; "Use
    Windows classic folders", saved as CommonTasks=false, takes the task
    pane out of the open window, the view then the whole width, and
    View > Explorer Bar > Folders shows the tree as the button does."""
    x, y, tasks = open_documents(s)
    left = (x + 8, y + 140, 190, 300)  # the task pane or the Folders tree
    away = (WIDTH - 24, HEIGHT - 80)
    s.inject("move", x + FOLDERS_BUTTON[0], y + FOLDERS_BUTTON[1], "click", "sleep", 300,
             "move", *away)
    tree = s.settle("folders")
    s.check(tree.differs(tasks, left) > 0.05, "the Folders button put the tree in the pane's place")
    s.inject("move", x + FOLDERS_BUTTON[0], y + FOLDERS_BUTTON[1], "click", "sleep", 300,
             "move", *away)
    back = s.settle("folders off")
    s.check(back.differs(tasks, left) < 0.02, "Folders off: the task pane is back")
    s.log("ok: the task pane at first, the Folders tree in its place and back")
    s.launch("xd-folders")
    s.wait_window(title="Folder Options", state="activated")
    # Below the folder window's caption, which goes inactive.
    below = (0, y + 40, WIDTH, HEIGHT - 80 - y)
    fx, fy, fw, fh = s.settle("folder options").changed_box(back, below)
    s.inject("key", "alt+l", "sleep", 300)
    s.settle("classic ticked")
    s.inject("move", fx + fw - 208, fy + fh - 21, "click")
    s.wait(lambda: s.ini().has_section("Folders")
           and s.ini()["Folders"].get("CommonTasks") == "false", "CommonTasks=false saved")  # fmt: skip
    s.log("ok: CommonTasks=false saved in [Folders]")
    s.wait_window(title="Folder Options", gone=True)
    s.inject("move", *away)
    s.wait(lambda: s.shot("classic").differs(tasks, left) > 0.05, "the task pane to go")
    classic = s.settle("classic folders")
    # Letters, the first item, now at the window's left edge.
    icon = (x + 30, y + 125, 60, 60)
    s.check(classic.differs(tasks, icon) > 0.05 and classic.differs(tree, icon) > 0.05,
            "the view took the pane's place")
    s.inject("key", "alt+v", "sleep", 300, "key", "e", "sleep", 300, "key", "o", "sleep", 300)
    s.check(s.settle("classic, folders").differs(tree, left) < 0.02,
            "View > Explorer Bar > Folders still shows the tree")
    s.inject("move", x + FOLDERS_BUTTON[0], y + FOLDERS_BUTTON[1], "click", "sleep", 300,
             "move", *away)
    s.check(s.settle("classic, folders off").differs(classic, left) < 0.02,
            "and Folders off, the whole width again")
    s.inject("move", x + LETTERS[0] - 210, y + LETTERS[1], "click", "sleep", 100, "click")
    files_title(s, "Letters", "Letters, at the left edge, to open")
    s.log("ok: classic folders: no task pane, the view the whole width")


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
    # With the Folders tree (white), not the task pane (a gradient, which
    # the outline's inverted pixels don't make black and white).
    s.launch("xd-files", "--explore", os.path.join(s.home, "Documents"))
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
    s.launch("xd-desk", "--page", "3")
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
    s.launch("xd-desk", "--page", "3")
    s.wait_window(title="Display Properties", state="activated")
    s.settle("appearance")
    # Tabs, style, scheme, font size, then Advanced.
    s.inject("key", "Tab", "key", "Tab", "key", "Tab", "key", "Tab", "sleep", 200, "key", "Return")
    s.wait_window(title="Window Color and Appearance", state="activated")
    s.settle("window-color")
    # Esc is Cancel: nothing changes.
    s.inject("key", "Escape")
    s.wait_window(title="Window Color and Appearance", gone=True)
    # Keys before Display Properties has the keyboard again are lost, and
    # Return would then be its OK.
    s.wait_window(title="Display Properties", state="activated")
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


def write_strip(path, w, h, colours):
    """A vertical image strip of solid frames (RGBA), as a package's PNG."""
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    pb = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, w, h * len(colours))
    for i, (r, g, b) in enumerate(colours):
        pb.new_subpixbuf(0, i * h, w, h).fill((r << 24) | (g << 16) | (b << 8) | 0xFF)
    pb.savev(path, "png", [], [])


def write_test_package(folder, name, variants):
    """A tiny theme package of our own: window captions and borders in one
    solid colour per variant (active), grey (inactive). variants: (id,
    colour name, (r, g, b))."""
    os.makedirs(os.path.join(folder, "styles"), exist_ok=True)
    os.makedirs(os.path.join(folder, "images"), exist_ok=True)
    xml = [f'<xdtheme version="1" name="{name}" base="classic">']
    for vid, label, rgb in variants:
        write_strip(os.path.join(folder, "images", f"{vid}-caption.png"), 12, 26,
                    [rgb, (128, 128, 128)])
        write_strip(os.path.join(folder, "images", f"{vid}-side.png"), 4, 8,
                    [rgb, (128, 128, 128)])
        write_strip(os.path.join(folder, "images", f"{vid}-bottom.png"), 8, 4,
                    [rgb, (128, 128, 128)])
        strip = 'ImageCount="2" ImageLayout="Vertical" SizingMargins="2 2 2 2" SizingType="Stretch"'
        with open(os.path.join(folder, "styles", f"{vid}.xml"), "w") as f:
            f.write(
                '<style><class name="Window">'
                f'<part name="Caption" ImageFile="images/{vid}-caption.png" {strip}/>'
                f'<part name="FrameLeft" ImageFile="images/{vid}-side.png" {strip}/>'
                f'<part name="FrameRight" ImageFile="images/{vid}-side.png" {strip}/>'
                f'<part name="FrameBottom" ImageFile="images/{vid}-bottom.png" {strip}/>'
                "</class></style>"
            )
        xml.append(f'<variant id="{vid}" name="{label}" size="Normal" style="styles/{vid}.xml"/>')
    xml.append("</xdtheme>")
    with open(os.path.join(folder, "theme.xml"), "w") as f:
        f.write("".join(xml))


def theme_packages_setup(s):
    write_test_package(os.path.join(s.data, "xeondesktop", "themes", "testpkg"), "Test Orange",
                       [("orange", "Orange", (255, 128, 0)), ("lime", "Lime", (0, 255, 0))])


def near(rgb, want, tolerance=12):
    return max(abs(p - q) for p, q in zip(rgb, want)) <= tolerance


@scenario(setup=theme_packages_setup)
def display_theme_package(s):
    """Display Properties lists an installed theme package in Windows and
    buttons; choosing it shows it in the preview, and OK writes ThemePackage
    and ThemeVariant: the compositor redraws an open window's frame in the
    package's look. Browse... shows xd-theme-import's error for a file that
    isn't a visual style, and installs and chooses a .xdtheme."""
    empty = s.settle("desktop")
    s.launch("xd-message", "Hello", "A window to watch.")
    s.wait_window(title="Hello", state="activated")
    hello = s.settle("hello")
    box = hello.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None, f"the window to watch is up at {box}")
    # Its caption, left of the close button, below the frame's top edge.
    cap = (box[0] + box[2] - 50, box[1] + 8)
    s.check(not near(hello.pixel(*cap), (255, 128, 0)), f"its caption isn't orange yet "
            f"{hello.pixel(*cap)}")

    s.launch("xd-desk", "--page", "3")
    s.wait_window(title="Display Properties", state="activated")
    props = s.settle("appearance")
    dp = props.changed_box(hello, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dp is not None, f"Display Properties is up at {dp}")
    # The client area (404 x 448) from the bottom right of what changed;
    # the preview is at 20, 40 (364 x 186).
    cx, cy = dp[0] + dp[2] - 3 - 404, dp[1] + dp[3] - 3 - 448
    preview = (cx + 30, cy + 50, 340, 160)
    # Windows and buttons, then its last item: the package.
    s.inject("key", "Tab", "sleep", 200, "key", "End", "sleep", 400)
    chosen = s.settle("package-chosen")

    def orange_share(shot, rect):
        x0, y0, w, h = rect
        n = hits = 0
        for y in range(y0, y0 + h, 3):
            for x in range(x0, x0 + w, 3):
                n += 1
                hits += near(shot.pixel(x, y), (255, 128, 0))
        return hits / max(n, 1)

    s.check(orange_share(chosen, preview) > 0.03,
            f"the preview draws the package's orange captions ({orange_share(chosen, preview):.3f})")
    # Color scheme lists the package's colours: Lime is its second.
    # Its list by mouse: the arrow, then the second row (17px each, below
    # the 21px box), then back to the first.
    combo = (cx + 20, cy + 304)
    s.inject("move", combo[0] + 210, combo[1] + 10, "click", "sleep", 400,
             "move", combo[0] + 60, combo[1] + 21 + 17 + 8, "click", "sleep", 400)
    lime = s.settle("lime")
    s.check(lime.differs(chosen, preview) > 0.02 and orange_share(lime, preview) < 0.01,
            "Color scheme's second colour, Lime, shows in the preview")
    s.inject("move", combo[0] + 210, combo[1] + 10, "click", "sleep", 400,
             "move", combo[0] + 60, combo[1] + 21 + 8, "click", "sleep", 400)
    s.check(orange_share(s.settle("orange-again"), preview) > 0.03, "and Orange again")
    s.inject("move", cx + 197, cy + 427, "click")  # OK: apply and close
    s.wait_window(title="Display Properties", gone=True)

    def written():
        a = s.ini()["Appearance"]
        return a.get("ThemePackage") == "testpkg" and a.get("ThemeVariant") == "orange"

    s.wait(written, "desktop.ini to name the package")
    s.log("ok: ThemePackage=testpkg, ThemeVariant=orange")

    def frame_orange():
        return near(s.shot("frame").pixel(*cap), (255, 128, 0))

    s.wait(frame_orange, "the open window's caption to turn orange")
    s.log("ok: the compositor drew the open window's frame from the package")

    # Browse... (at 298, 255 in the client area, 86 x 23): a file that
    # isn't a visual style.
    bad = os.path.join(s.home, "Downloads", "broken.msstyles")
    with open(bad, "wb") as f:
        f.write(b"MZ but not really a visual style")
    before = s.settle("before-second")
    s.launch("xd-desk", "--page", "3", env={"XD_CHOOSE_FILE_ANSWER": bad})
    s.wait_window(title="Display Properties", state="activated")
    dp = s.settle("appearance-again").changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dp is not None, f"Display Properties is up again at {dp}")
    cx, cy = dp[0] + dp[2] - 3 - 404, dp[1] + dp[3] - 3 - 448
    s.inject("move", cx + 341, cy + 266, "click")

    def error_box():
        return len([w for w in s.windows() if w["title"] == "Display Properties"]) == 2

    s.wait(error_box, "the error message box", timeout=40)
    s.settle("import-error")
    if importlib.util.find_spec("pefile") is None:
        s.log("skip: python-pefile isn't installed, so the error is xd-theme-import's "
              "\"needs pefile\": importing a .msstyles isn't tested")
    else:
        s.log("ok: xd-theme-import's error for a bad .msstyles is in a message box")
    s.check(not any(n.endswith(".part") or n.startswith("broken")
                    for n in os.listdir(os.path.join(s.data, "xeondesktop", "themes"))),
            "nothing is left of the failed import")
    s.inject("key", "Return")  # the box's OK
    s.wait(lambda: not error_box(), "the message box to close")
    s.inject("move", cx + 278, cy + 427, "click")  # Cancel
    s.wait_window(title="Display Properties", gone=True)

    # Browse...: a package (.xdtheme), installed and chosen.
    import zipfile

    src = os.path.join(s.base, "bluebox")
    write_test_package(src, "Blue Box", [("blue", "Blue", (0, 0, 255))])
    package = os.path.join(s.home, "Downloads", "bluebox.xdtheme")
    with zipfile.ZipFile(package, "w") as z:
        for folder, _, files in os.walk(src):
            for name in files:
                full = os.path.join(folder, name)
                z.write(full, os.path.relpath(full, src))
    before = s.settle("before-third")
    s.launch("xd-desk", "--page", "3", env={"XD_CHOOSE_FILE_ANSWER": package})
    s.wait_window(title="Display Properties", state="activated")
    third = s.settle("appearance-third")
    dp = third.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dp is not None, f"Display Properties is up a third time at {dp}")
    cx, cy = dp[0] + dp[2] - 3 - 404, dp[1] + dp[3] - 3 - 448
    s.inject("move", cx + 341, cy + 266, "click", "sleep", 1000)
    installed = os.path.join(s.data, "xeondesktop", "themes", "bluebox.xdtheme")
    s.wait(lambda: os.path.exists(installed), "the package to be installed")
    s.settle("installed")
    s.inject("move", cx + 197, cy + 427, "click")  # OK
    s.wait_window(title="Display Properties", gone=True)
    s.wait(lambda: s.ini()["Appearance"].get("ThemePackage") == "bluebox",
           "desktop.ini to name the installed package")
    s.log("ok: Browse... installed bluebox.xdtheme and chose it")
    s.wait(lambda: near(s.shot("blue-frame").pixel(*cap), (0, 0, 255)),
           "the open window's caption to turn blue")
    s.log("ok: and the frame changed again, the old package freed")


def write_wav(path, hz):
    """A short tone of our own (16-bit mono), with the standard library's wave."""
    import math
    import struct
    import wave

    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * hz * i / 8000)))
                               for i in range(800)))


def write_xcursor(path, size, rgba, hot):
    """An XCursor file of one solid square, written with libXcursor."""
    import ctypes
    import ctypes.util

    class Image(ctypes.Structure):
        _fields_ = [("version", ctypes.c_uint), ("size", ctypes.c_uint),
                    ("width", ctypes.c_uint), ("height", ctypes.c_uint),
                    ("xhot", ctypes.c_uint), ("yhot", ctypes.c_uint),
                    ("delay", ctypes.c_uint), ("pixels", ctypes.POINTER(ctypes.c_uint32))]

    class Images(ctypes.Structure):
        _fields_ = [("nimage", ctypes.c_int), ("images", ctypes.POINTER(ctypes.POINTER(Image))),
                    ("name", ctypes.c_char_p)]

    lib = ctypes.CDLL(ctypes.util.find_library("Xcursor") or "libXcursor.so.1")
    lib.XcursorImageCreate.restype = ctypes.POINTER(Image)
    lib.XcursorImagesCreate.restype = ctypes.POINTER(Images)
    lib.XcursorFilenameSaveImages.argtypes = [ctypes.c_char_p, ctypes.POINTER(Images)]
    img = lib.XcursorImageCreate(size, size)
    img.contents.xhot, img.contents.yhot = hot
    r, g, b, a = rgba
    for i in range(size * size):
        img.contents.pixels[i] = (a << 24) | (r << 16) | (g << 8) | b
    images = lib.XcursorImagesCreate(1)
    images.contents.images[0] = img
    images.contents.nimage = 1
    ok = lib.XcursorFilenameSaveImages(path.encode(), images)
    lib.XcursorImagesDestroy(images)
    if not ok:
        raise Failure(f"libXcursor couldn't write {path}")


def write_test_theme(folder, name, rgb):
    """A theme package that brings more than a style: a wallpaper of one
    colour (tiled), a sound scheme with two sounds of its own, and an
    XCursor theme. All made here."""
    write_test_package(folder, name, [("plain", "Plain", (90, 90, 90))])
    os.makedirs(os.path.join(folder, "wallpaper"))
    write_png(os.path.join(folder, "wallpaper", "tile.png"), 32, 32,
              (rgb[0] << 16) | (rgb[1] << 8) | rgb[2])
    os.makedirs(os.path.join(folder, "sounds"))
    write_wav(os.path.join(folder, "sounds", "logon.wav"), 440)
    write_wav(os.path.join(folder, "sounds", "max.wav"), 880)
    with open(os.path.join(folder, "sounds", "scheme.ini"), "w") as f:
        f.write("[Sounds]\nScheme=default\n\n[Events]\nWindowsLogon=sounds/logon.wav\n"
                "Maximize=sounds/max.wav\nCriticalStop=\n")
    os.makedirs(os.path.join(folder, "cursors", "cursors"))
    with open(os.path.join(folder, "cursors", "index.theme"), "w") as f:
        f.write(f"[Icon Theme]\nName={name} pointers\nInherits=default\n")
    write_xcursor(os.path.join(folder, "cursors", "cursors", "left_ptr"), 24, (255, 0, 255, 255),
                  (1, 1))
    with open(os.path.join(folder, "theme.xml")) as f:
        xml = f.read()
    with open(os.path.join(folder, "theme.xml"), "w") as f:
        f.write(xml.replace("</xdtheme>", '<wallpaper file="wallpaper/tile.png" style="tile"/>'
                            '<sounds scheme="sounds/scheme.ini"/><cursors theme="cursors"/>'
                            "</xdtheme>"))


def themes_tab_setup(s):
    themes = os.path.join(s.data, "xeondesktop", "themes")
    write_test_theme(os.path.join(themes, "testtheme"), "Test Theme", (200, 40, 160))
    # A package with no more than a style: not a theme.
    write_test_package(os.path.join(themes, "styleonly"), "Style Only",
                       [("green", "Green", (0, 200, 0))])


@scenario(setup=themes_tab_setup)
def display_themes_tab(s):
    """Display Properties opens on XP's Themes tab. Its list holds the
    packages that bring more than a style, after Windows XP and Windows
    Classic; choosing one shows its wallpaper in the Sample, and OK writes
    the package, the wallpaper, the sound scheme and the pointers, each
    where the settings keep it, with the files unpacked there. Save As...
    writes a .xdtheme of the current settings, Delete puts it in the Trash,
    and Windows Classic takes the wallpaper and pointers back."""
    data = s.data
    files = os.path.join(data, "xeondesktop", "theme-files", "testtheme")
    before = s.settle("desktop")
    s.launch("xd-desk")
    s.wait_window(title="Display Properties", state="activated")
    props = s.settle("themes")
    dp = props.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dp is not None, f"Display Properties is up at {dp}")
    cx, cy = dp[0] + dp[2] - 3 - 404, dp[1] + dp[3] - 3 - 448
    sample = (cx + 30, cy + 160, 340, 220)
    magenta = (200, 40, 160)
    s.check(share_like(props, sample, magenta) < 0.01, "the Sample has no magenta yet")
    # The list (Theme: at 20, 98, 206 x 21; 17px rows below it) holds
    # Windows XP, Windows Classic (this classic session), the package and
    # Browse...: Style Only brings nothing but a style.
    combo = (cx + 20, cy + 98)
    s.inject("move", combo[0] + 196, combo[1] + 10, "click", "sleep", 400,
             "move", combo[0] + 60, combo[1] + 21 + 2 * 17 + 8, "click", "sleep", 600)
    chosen = s.settle("theme-chosen")
    s.check(share_like(chosen, sample, magenta) > 0.2,
            f"the Sample shows the theme's wallpaper ({share_like(chosen, sample, magenta):.2f})")
    s.check(os.path.exists(os.path.join(files, "wallpaper", "tile.png")),
            "choosing it unpacked its wallpaper")
    s.inject("move", cx + 197, cy + 427, "click")  # OK
    s.wait_window(title="Display Properties", gone=True)

    def written():
        c = s.ini()
        return (c["Appearance"].get("ThemePackage") == "testtheme" and
                c["Desktop"].get("Wallpaper") == os.path.join(files, "wallpaper", "tile.png") and
                c["Desktop"].get("WallpaperMode") == "tile" and
                c["Mouse"].get("CursorTheme") == "xdtheme-testtheme")

    s.wait(written, "desktop.ini to take the theme")
    s.log("ok: ThemePackage, Wallpaper (unpacked, tiled) and CursorTheme written")
    sounds = s.ini("sounds.ini")["Events"]
    s.check(sounds.get("WindowsLogon") == os.path.join(files, "sounds", "logon.wav") and
            sounds.get("Maximize") == os.path.join(files, "sounds", "max.wav") and
            sounds.get("CriticalStop") == "" and sounds.get("Exclamation") == "dialog-warning",
            "sounds.ini has the theme's sounds, no Critical Stop, and Windows Default's others")
    for f in ("logon.wav", "max.wav"):
        s.check(os.path.getsize(os.path.join(files, "sounds", f)) > 1000, f"{f} is unpacked")
    s.check(os.path.exists(os.path.join(data, "icons", "xdtheme-testtheme", "cursors",
                                        "left_ptr")) and
            os.path.exists(os.path.join(data, "icons", "xdtheme-testtheme", "index.theme")),
            "the pointers are installed as an XCursor theme")
    s.wait(lambda: share_like(s.shot("wallpaper"), (300, 200, 400, 300), magenta) > 0.9,
           "the desktop to show the theme's wallpaper")
    s.log("ok: the desktop shows it")

    # Save As... (232, 97 in the client area): the current settings as a
    # package among the user's themes, then chosen; Delete (309, 97) puts
    # it in the Trash.
    saved = os.path.join(data, "xeondesktop", "themes", "Saved.xdtheme")
    before = s.settle("before-second")
    s.launch("xd-desk", env={"XD_CHOOSE_FILE_ANSWER": saved})
    s.wait_window(title="Display Properties", state="activated")
    dp = s.settle("second").changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dp is not None, f"Display Properties is up again at {dp}")
    cx, cy = dp[0] + dp[2] - 3 - 404, dp[1] + dp[3] - 3 - 448
    s.inject("move", cx + 269, cy + 108, "click")
    s.wait(lambda: os.path.exists(saved), "Save As... to write Saved.xdtheme")
    import zipfile

    with zipfile.ZipFile(saved) as z:
        names = z.namelist()
        xml = z.read("theme.xml").decode()
        scheme = z.read("sounds/scheme.ini").decode()
    s.check(names[0] == "theme.xml" and 'name="Saved"' in xml and
            '<wallpaper file="wallpaper/tile.png" style="tile"/>' in xml and
            '<cursors theme="cursors"/>' in xml and "cursors/cursors/left_ptr" in names and
            "sounds/logon.wav" in names and "WindowsLogon=sounds/logon.wav" in scheme and
            "styles/plain.xml" in names, f"Saved.xdtheme holds the look: {names}")
    s.settle("saved")
    s.inject("move", cx + 346, cy + 108, "click", "sleep", 500)
    s.wait(lambda: not os.path.exists(saved), "Delete to remove Saved.xdtheme")
    trash = os.path.join(data, "Trash", "files")
    s.check(os.path.isdir(trash) and any(n.startswith("Saved") for n in os.listdir(trash)),
            "it's in the Trash")
    s.inject("move", cx + 278, cy + 427, "click")  # Cancel
    s.wait_window(title="Display Properties", gone=True)
    s.check(s.ini()["Appearance"].get("ThemePackage") == "testtheme",
            "Cancel left the applied theme")

    # Windows Classic: the list's second item.
    before = s.settle("before-third")
    s.launch("xd-desk")
    s.wait_window(title="Display Properties", state="activated")
    dp = s.settle("third").changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dp is not None, f"Display Properties is up a third time at {dp}")
    cx, cy = dp[0] + dp[2] - 3 - 404, dp[1] + dp[3] - 3 - 448
    sample = (cx + 30, cy + 160, 340, 220)
    combo = (cx + 20, cy + 98)
    s.inject("move", combo[0] + 196, combo[1] + 10, "click", "sleep", 400,
             "move", combo[0] + 60, combo[1] + 21 + 17 + 8, "click", "sleep", 600)
    s.wait(lambda: share_like(s.shot("classic"), sample, magenta) < 0.01,
           "the Sample to lose the theme's wallpaper")
    s.inject("move", cx + 197, cy + 427, "click")  # OK
    s.wait_window(title="Display Properties", gone=True)

    def classic():
        c = s.ini()
        return (c["Appearance"].get("ThemePackage") == "" and
                c["Appearance"].get("Style", "").lower() == "classic" and
                c["Desktop"].get("Wallpaper") == "" and c["Mouse"].get("CursorTheme") == "")

    s.wait(classic, "Windows Classic to clear the package, wallpaper and pointers")
    s.log("ok: Windows Classic took them back")


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
    s.launch("xd-desk", "--page", "3")
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
    s.launch("xd-desk", "--page", "3")
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
    s.launch("xd-desk", "--page", "3")
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


@scenario(LUNA_INI)
def task_manager_cpu_graphs(s):
    """Task Manager's Performance tab with One Graph Per CPU: a graph for
    each CPU, side by side, where the one graph for all of them was."""
    folder = os.path.join(os.environ["XDG_STATE_HOME"], "xeondesktop")
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "taskmgr.ini"), "w") as f:
        f.write("[TaskManager]\nTab=2\nOneGraphPerCPU=true\n")
    before = s.settle("desktop")
    s.launch("xd-taskmgr")
    s.wait_window(title="Task Manager", state="activated")
    time.sleep(2)  # a couple of samples
    shot = s.settle("performance")
    box = shot.changed_box(before, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None, f"Task Manager is on screen ({box})")

    def graphs(shot):
        """The dark graphs across the CPU history's top row, from the
        window's top left (see build_performance)."""
        y = box[1] + 106
        count, run = 0, 0
        for x in range(box[0] + 115, box[0] + box[2] - 10):
            if max(shot.pixel(x, y)) < 140:
                run += 1
            else:
                count += run >= 10
                run = 0
        return count + (run >= 10)

    cpus = os.cpu_count() or 1
    want = cpus if cpus <= 12 else 2
    found = s.wait(lambda: (lambda n: n if n >= want else None)(graphs(s.shot("graphs"))),
                   f"{want} CPU graphs")
    s.log(f"ok: {found} CPU graphs for {cpus} CPUs")


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


UPS_PATH = "/org/freedesktop/UPower/devices/ups_hiddev0"


def mock_ups(s):
    """Before the compositor starts: a private system bus with dbusmock's
    UPower (a desktop: no battery, a USB UPS on mains, fully charged) and
    logind (logging its calls)."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    DBusTestCase.start_system_bus()
    for template in ("upower", "logind"):
        log = open(os.path.join(s.base, template + ".log"), "w")
        proc, _ = DBusTestCase.spawn_server_template(template, {}, stdout=log, system_bus=True)
        s.children.append(proc)
    upower = dbus.SystemBus().get_object("org.freedesktop.UPower", "/org/freedesktop/UPower")
    mock = dbus.Interface(upower, MOCK_IFACE)
    mock.AddObject(UPS_PATH, "org.freedesktop.UPower.Device", {
        "Type": dbus.UInt32(3), "PowerSupply": dbus.Boolean(True),
        "IsPresent": dbus.Boolean(True), "State": dbus.UInt32(4),
        "Percentage": dbus.Double(100.0), "TimeToEmpty": dbus.Int64(2280),
        "WarningLevel": dbus.UInt32(1), "Vendor": dbus.String("American Power Conversion"),
        "Model": dbus.String("Back-UPS ES 700"), "NativePath": dbus.String("hiddev0"),
    }, [])  # fmt: skip
    mock.EmitSignal("org.freedesktop.UPower", "DeviceAdded", "o", [UPS_PATH])


@scenario(LUNA_INI, requires=("dbusmock",), setup=mock_ups)
def power_ups(s):
    """Power Options' UPS tab (a dbusmock UPower UPS): with no UPS chosen it
    shows (None); Select... lists UPower's UPS, and choosing it (Finish,
    Apply) saves [UPS] Device and shows its status and maker."""
    from dbusmock import DBusTestCase

    try:
        empty = s.settle("desktop")
        s.launch("xd-power", "--page", "3")
        s.wait_window(title="Power Options Properties", state="activated")
        tab = s.settle("ups-tab")
        x, y, _, _ = tab.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        s.check(not s.ini().has_option("UPS", "Device") or not s.ini()["UPS"]["Device"],
                "no UPS chosen to start with")  # fmt: skip
        # Select... (UPS Selection is a dialog box of the window, not on
        # the taskbar): (None) first, UPower's UPS below it.
        s.inject("move", x + 153, y + 256, "click", "sleep", 1500)
        selection = s.settle("selection")
        s.check(selection.differs(tab, (x + 30, y + 100, 360, 200)) > 0.2,
                "Select... opened UPS Selection")  # fmt: skip
        # The UPS (Down from (None)); Enter on the row is Finish.
        s.inject("key", "Down", "sleep", 200, "key", "Return", "sleep", 800)
        chosen = s.settle("chosen")
        s.check(chosen.differs(tab, (x + 110, y + 90, 200, 140)) > 0.02,
                "the tab shows the UPS's status and maker")  # fmt: skip
        s.inject("key", "alt+a")

        def saved():
            ini = s.ini()
            return ini.has_section("UPS") and ini["UPS"].get("Device")

        s.check(s.wait(saved, "[UPS] Device saved") == "upower:" + UPS_PATH,
                "Apply saved the UPS chosen")  # fmt: skip
        # Configure...: UPS Configuration, centred on the window. "Minutes
        # on battery before critical alarm" (Alt+M), OK, Apply.
        s.inject("move", x + 70, y + 256, "click", "sleep", 1500)
        configuration = s.settle("configuration")
        s.check(configuration.differs(chosen, (x + 20, y + 60, 380, 300)) > 0.2,
                "Configure... opened UPS Configuration")  # fmt: skip
        s.inject("key", "alt+m", "sleep", 300, "move", x + 299, y + 394, "click", "sleep", 800)
        s.inject("key", "alt+a")

        def critical_after():
            return s.ini()["UPS"].get("CriticalAfterMinutes") == "true"

        s.wait(critical_after, "[UPS] CriticalAfterMinutes saved")
        s.log("ok: UPS Configuration's minutes on battery saved")
    finally:
        DBusTestCase.tearDownClass()


@scenario(LUNA_INI + f"\n[UPS]\nDevice=upower:{UPS_PATH}\nFirstNotification=0\n"
          "RunProgram=true\nProgram=touch \"$HOME/ups-program-ran\"\n",
          requires=("dbusmock",), setup=mock_ups)  # fmt: skip
def ups_power_failure(s):
    """The shell watching the dbusmock UPS (Power Options' [UPS]): the power
    failing shows "Power Failure", its return "Power Restored"; failing
    again and the UPS's battery running low (UPower's WarningLevel) is the
    critical alarm: the program runs, then logind's PowerOff (dbusmock's,
    only logged)."""
    import dbus
    from dbusmock import DBusTestCase, MOCK_IFACE

    ran = os.path.join(s.home, "ups-program-ran")

    def logind_called(method):
        with open(os.path.join(s.base, "logind.log")) as f:
            return any(f" {method} " in line for line in f)

    def power_failures():
        return sum(w["title"] == "Power Failure" for w in s.windows())

    try:
        upower = dbus.SystemBus().get_object("org.freedesktop.UPower", "/org/freedesktop/UPower")
        mock = dbus.Interface(upower, MOCK_IFACE)
        s.settle("desktop")
        # Discharging: UPower's UPS warning goes to Discharging too.
        on_battery = {"State": dbus.UInt32(2), "WarningLevel": dbus.UInt32(2)}
        mock.SetDeviceProperties(UPS_PATH, on_battery)
        s.wait_window(title="Power Failure")
        s.settle("power-failure")
        s.log("ok: the power failing showed Power Failure")
        mock.SetDeviceProperties(UPS_PATH, {"State": dbus.UInt32(1), "WarningLevel": dbus.UInt32(1)})
        s.wait_window(title="Power Restored")
        s.log("ok: its return showed Power Restored")
        s.check(not logind_called("PowerOff") and not os.path.exists(ran),
                "no critical alarm yet")  # fmt: skip
        mock.SetDeviceProperties(UPS_PATH, on_battery)
        s.wait(lambda: power_failures() >= 2, "a second Power Failure")
        mock.SetDeviceProperties(UPS_PATH, {"WarningLevel": dbus.UInt32(3)})
        s.wait_window(title="UPS Critical Alarm")
        s.wait(lambda: os.path.exists(ran), "the critical alarm's program")
        s.log("ok: at the UPS's low battery the critical alarm ran its program")
        s.wait(lambda: logind_called("PowerOff"), "logind's PowerOff")
        s.log("ok: then shut down (logind's PowerOff, a mock's)")
    finally:
        DBusTestCase.tearDownClass()


class FakeUpsd:
    """A stand-in for NUT's upsd on 127.0.0.1 (a free port): the network
    protocol's commands libupsclient sends (GET VAR, LIST UPS, LIST VAR,
    LOGOUT...), for one UPS whose variables the scenario changes."""

    def __init__(self, name, description, variables):
        import socketserver

        self.name, self.description = name, description
        self.variables = dict(variables)
        self.lock = threading.Lock()
        fake = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                for raw in self.rfile:
                    words = shlex.split(raw.decode(errors="replace"))
                    reply = fake.answer(words)
                    if reply is None:
                        break
                    self.wfile.write("".join(line + "\n" for line in reply).encode())
                    if words[:1] == ["LOGOUT"]:
                        break

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self.server = Server(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def set(self, **variables):
        with self.lock:
            self.variables.update({k.replace("_", "."): v for k, v in variables.items()})

    def answer(self, words):
        def quote(value):
            return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'

        with self.lock:
            if not words:
                return None
            if words[:2] == ["GET", "VAR"] and len(words) == 4:
                if words[2] != self.name:
                    return ["ERR UNKNOWN-UPS"]
                if words[3] not in self.variables:
                    return ["ERR VAR-NOT-SUPPORTED"]
                return [f"VAR {self.name} {words[3]} {quote(self.variables[words[3]])}"]
            if words == ["LIST", "UPS"]:
                return ["BEGIN LIST UPS", f"UPS {self.name} {quote(self.description)}",
                        "END LIST UPS"]  # fmt: skip
            if words[:2] == ["LIST", "VAR"] and len(words) == 3 and words[2] == self.name:
                return ([f"BEGIN LIST VAR {self.name}"]
                        + [f"VAR {self.name} {k} {quote(v)}" for k, v in self.variables.items()]
                        + [f"END LIST VAR {self.name}"])  # fmt: skip
            if words[0] == "LOGOUT":
                return ["OK Goodbye"]
            if words[0] in ("VER", "NETVER", "PROTVER"):
                return ["1.3" if words[0] != "VER" else "Network UPS Tools upsd 2.8.2"]
            if words[0] == "STARTTLS":
                return ["ERR FEATURE-NOT-CONFIGURED"]
            return ["ERR UNKNOWN-COMMAND"]

    def close(self):
        self.server.shutdown()
        self.server.server_close()


@scenario(LUNA_INI, requires=("dbusmock",), setup=mock_ups)
def nut_ups(s):
    """A UPS on NUT (a fake upsd, spoken to through libupsclient): UPS
    Selection finds it on the server typed, the tab shows its status, and
    once applied the shell watches it: on battery (OB) "Power Failure",
    then the critical alarm at its low battery (LB): logind's PowerOff
    (dbusmock's, only logged)."""
    from dbusmock import DBusTestCase

    upsd = FakeUpsd("rack", "Server room", {
        "ups.status": "OL CHRG", "battery.charge": "87", "battery.runtime": "1500",
        "ups.mfr": "Eaton", "ups.model": "5E 1100i",
    })  # fmt: skip
    server = f"127.0.0.1:{upsd.port}"

    def logind_called(method):
        with open(os.path.join(s.base, "logind.log")) as f:
            return any(f" {method} " in line for line in f)

    try:
        empty = s.settle("desktop")
        s.launch("xd-power", "--page", "3")
        s.wait_window(title="Power Options Properties", state="activated")
        tab = s.settle("ups-tab")
        x, y, _, _ = tab.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
        s.inject("move", x + 153, y + 256, "click", "sleep", 1500)
        s.settle("selection")
        # The server typed, Find; the last row is its UPS; Enter is Finish.
        s.inject("key", "alt+v", "key", "ctrl+a", "type", server, "key", "alt+f", "sleep", 1000)
        s.inject("key", "alt+s", "key", "End", "sleep", 200)
        s.settle("found")
        s.inject("key", "Return", "sleep", 1000)
        shown = s.settle("nut-status")
        s.check(shown.differs(tab, (x + 110, y + 90, 200, 140)) > 0.02,
                "the tab shows the NUT UPS's status and maker")  # fmt: skip
        s.inject("key", "alt+a")

        def saved():
            ini = s.ini()
            return ini.has_section("UPS") and ini["UPS"].get("Device")

        s.check(s.wait(saved, "[UPS] Device saved") == f"nut:rack@{server}",
                "Apply saved NUT's UPS")  # fmt: skip
        upsd.set(ups_status="OB DISCHRG", battery_runtime="1200")
        s.wait_window(title="Power Failure", timeout=30)
        s.log("ok: NUT's OB showed Power Failure")
        s.check(not logind_called("PowerOff"), "no critical alarm yet")
        upsd.set(ups_status="OB DISCHRG LB", battery_charge="9")
        s.wait(lambda: logind_called("PowerOff"), "logind's PowerOff", timeout=30)
        s.log("ok: NUT's LB was the critical alarm: shut down (logind's PowerOff, a mock's)")
    finally:
        upsd.close()
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
    # Classic's taskbar alone (28px): above it the windows' status bars
    # show the disk's free space, which may change meanwhile.
    bar = (0, HEIGHT - 28, WIDTH, 28)
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
            ' ImageCount="5" ImageLayout="Vertical" SizingMargins="4 4 4 4" ContentMargins="3 3 3 3"'
            ' SizingType="Stretch" TextColor="0 0 0"/>\n'
            '    <part name="CheckBox" BgType="ImageFile" ImageFile="images/check.png"'
            ' ImageCount="12" ImageLayout="Vertical" SizingType="TrueSize"/>\n'
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
    shortcut = os.path.join(s.home, "Desktop", "Shortcut to org.xeondesktop.Files.desktop")
    s.wait(lambda: os.path.exists(shortcut), "the program's shortcut on the desktop")
    s.log("ok: Send To > Desktop (create shortcut) put it there")
    # A link to the installed launcher, which the desktop trusts to run
    # (a copy wouldn't be).
    s.check(os.path.islink(shortcut), "the shortcut is a link to the program's launcher")


@scenario(LUNA_INI)
def desktop_untrusted_shortcut(s):
    """A launcher on the desktop that's neither an installed program's nor
    executable (as a downloaded one): opening it asks first, showing what
    it runs, and Trust and Launch makes it executable and starts it. Then
    it runs without asking."""
    marker = os.path.join(s.base, "launched")
    shortcut = os.path.join(s.home, "Desktop", "zz-shortcut.desktop")
    with open(shortcut, "w") as f:
        f.write(f"[Desktop Entry]\nType=Application\nName=Zz Program\nExec=touch {marker}\n")
    os.chmod(shortcut, 0o644)
    s.settle("desktop")
    s.inject("move", WIDTH // 2, HEIGHT // 2, "click", "sleep", 300, "type", "zz", "key", "Return")
    s.wait_window(title="Untrusted Shortcut", state="activated")
    s.check(not os.path.exists(marker), "nothing ran before asking")
    s.log("ok: the untrusted shortcut asked first")
    s.inject("key", "alt+t")
    s.wait(lambda: os.path.exists(marker), "Trust and Launch to run it")
    s.check(os.stat(shortcut).st_mode & 0o100, "trusting made it executable")
    s.log("ok: Trust and Launch marked it trusted and ran it")
    os.remove(marker)
    s.wait_window(title="Untrusted Shortcut", gone=True)
    s.inject("move", WIDTH // 2, HEIGHT // 2, "click", "sleep", 300, "type", "zz", "key", "Return")
    s.wait(lambda: os.path.exists(marker), "the trusted shortcut to run")
    s.check(s.window(title="Untrusted Shortcut") is None, "a trusted one runs without asking")
    s.log("ok: once trusted, it runs straight away")


def clipboard_picture(s, label):
    """The clipboard's PNG (read with xd-paste) as a Shot, or None."""
    r = subprocess.run([s.tool("xd-paste"), "image/png"], stdout=subprocess.PIPE, timeout=10)
    if r.returncode != 0 or not r.stdout.startswith(b"\x89PNG"):
        return None
    path = os.path.join(s.base, "shots", f"clipboard-{label}.png")
    with open(path, "wb") as f:
        f.write(r.stdout)
    return Shot(path)


def wait_washed(s, desktop, point):
    """Waits for the Snipping Tool's white wash over the frozen screen."""
    def washed():
        before, now = desktop.pixel(*point), s.shot("washed").pixel(*point)
        return sum(now) > sum(before) + 60

    s.wait(washed, "the Snipping Tool's overlay")


@scenario()
def snipping_tool(s):
    """The Snipping Tool: a rectangular snip of the desktop (red selection
    ink at its edge) opens in the Mark-up window and is on the clipboard,
    and stays there once the Snipping Tool has closed; a window snip takes
    the window clicked; Esc cancels; Print Screen copies the screen."""
    desktop = s.settle("desktop")
    snip = s.launch(s.tool("xd-snip"), "--new=rectangle")
    wait_washed(s, desktop, (400, 300))
    s.inject("move", 100, 100, "down", "move", 200, 160, "move", 300, 250, "up")
    s.wait_window(app_id="org.xeondesktop.SnippingTool", state="activated")
    picture = s.wait(lambda: clipboard_picture(s, "rectangle"), "the snip on the clipboard")
    s.check((picture.width, picture.height) == (200, 150), f"a 200x150 snip ({picture.width}x{picture.height})")
    s.check(near(picture.pixel(100, 75), desktop.pixel(200, 175)), "the snip is the desktop there")
    s.check(near(picture.pixel(0, 75), (255, 0, 0)), f"red selection ink at its edge ({picture.pixel(0, 75)})")
    s.log("ok: a rectangular snip, in the Mark-up window and on the clipboard")
    # Closing asks to save it first (a dialog: no taskbar button, so seen
    # on the screen); No closes, and the snip stays copied.
    editor = s.settle("mark-up")
    s.inject("key", "alt+F4")
    wait_change(s, editor, (0, 0, WIDTH, HEIGHT - 40), "the question about saving", "question")
    s.inject("key", "alt+n")
    s.wait(lambda: snip.poll() is not None, "the Snipping Tool to close")
    s.check(clipboard_picture(s, "after") is not None, "the snip outlives the Snipping Tool")
    s.log("ok: closed without saving; the snip is still on the clipboard")

    # Window snip: the toolbar window of another Snipping Tool.
    toolbar = s.launch(s.tool("xd-snip"))
    s.wait_window(title="Snipping Tool", state="activated")
    _, box = wait_change(s, desktop, (0, 0, WIDTH, HEIGHT - 40), "the toolbar window", "toolbar")
    window_snip = s.launch(s.tool("xd-snip"), "--new=window")
    wait_washed(s, desktop, (WIDTH - 50, 50))
    s.inject("move", box[0] + box[2] // 2, box[1] + box[3] // 2, "click")
    def window_copied():
        shot = clipboard_picture(s, "window")
        return shot if shot is not None and (shot.width, shot.height) != (200, 150) else None
    picture = s.wait(window_copied, "the window snip on the clipboard")
    s.check(abs(picture.width - box[2]) <= 3 and abs(picture.height - box[3]) <= 3,
            f"the window snip is the window ({picture.width}x{picture.height}, window {box})")
    s.log("ok: a window snip takes the window clicked")
    editor = s.settle("window-mark-up")
    s.inject("key", "alt+F4")
    wait_change(s, editor, (0, 0, WIDTH, HEIGHT - 40), "the question about saving", "question")
    s.inject("key", "alt+n")
    s.wait(lambda: window_snip.poll() is not None, "the second Snipping Tool to close")

    # Esc cancels, and with --new nothing stays.
    cancelled = s.launch(s.tool("xd-snip"), "--new=free-form")
    wait_washed(s, desktop, (WIDTH - 50, 50))
    s.inject("key", "Escape")
    s.wait(lambda: cancelled.poll() is not None, "Esc to cancel the snip")
    s.log("ok: Esc cancels")
    toolbar.terminate()

    # Print Screen: the whole screen to the clipboard, no window.
    s.inject("key", "Print")
    def screen_copied():
        shot = clipboard_picture(s, "screen")
        return shot if shot is not None and shot.width == WIDTH else None
    picture = s.wait(screen_copied, "Print Screen to copy the screen")
    s.check(picture.height == HEIGHT, f"the whole screen ({picture.width}x{picture.height})")
    s.log("ok: Print Screen copies the screen")


def media_info(path):
    """(width, height, seconds) of a video file, from GStreamer's
    discoverer, or None if it can't be read."""
    import gi

    gi.require_version("Gst", "1.0")
    gi.require_version("GstPbutils", "1.0")
    from gi.repository import Gst, GstPbutils

    Gst.init(None)
    try:
        info = GstPbutils.Discoverer.new(10 * Gst.SECOND).discover_uri("file://" + path)
    except Exception:
        return None
    videos = info.get_video_streams()
    if not videos:
        return None
    return (videos[0].get_width(), videos[0].get_height(), info.get_duration() / Gst.SECOND)


@scenario()
def screen_recording(s):
    """Screen recording: Win+Shift+R's area over the frozen screen, a red
    frame around it while it records, the shortcut again stops it, and the
    MP4 in Videos/Screen Recordings is that area, as long as it ran."""
    for element in ("mp4mux", "x264enc"):
        if subprocess.run(["gst-inspect-1.0", "--exists", element]).returncode != 0:
            raise Failure(f"GStreamer's {element} is missing (gst-plugins-good, gst-plugins-ugly)")
    folder = os.path.join(s.home, "Videos", "Screen Recordings")
    desktop = s.settle("desktop")
    s.launch(s.tool("xd-snip"), "--record=area")
    wait_washed(s, desktop, (WIDTH - 50, 50))
    s.inject("move", 200, 150, "down", "move", 400, 300, "move", 520, 390, "up")
    started = time.monotonic()

    def framed(shot):
        """Red dashes just left of the area."""
        return any(near(shot.pixel(199, y), (230, 26, 26), 40) for y in range(150, 390, 2))

    shot = s.wait(lambda: (lambda shot: shot if framed(shot) else None)(s.shot("recording")),
                  "the red frame around the area")
    s.check(not near(shot.pixel(260, 260), (230, 26, 26), 40), "the area itself isn't framed")
    s.log("ok: a red frame stands around the area")
    s.wait(lambda: os.path.isdir(folder) and os.listdir(folder), "the recording's file")
    # Something to see: the pointer moving about inside the area.
    for x in range(250, 500, 25):
        s.inject("move", x, 280, "sleep", 100)
    while time.monotonic() - started < 2.5:
        time.sleep(0.2)
    s.launch(s.tool("xd-snip"), "--record")
    path = os.path.join(folder, os.listdir(folder)[0])
    info = s.wait(lambda: media_info(path), "a finished MP4", timeout=30)
    width, height, seconds = info
    s.check((width, height) == (320, 240), f"the video is the area ({width}x{height})")
    s.check(seconds >= 2, f"it lasts as long as it ran ({seconds:.1f} s)")
    s.log(f"ok: {os.path.basename(path)}: {width}x{height}, {seconds:.1f} s")
    s.wait(lambda: not framed(s.shot("frame-gone")), "the red frame to go")
    s.log("ok: stopping took the frame away")


@scenario()
def minefield(s):
    """Minefield: left click opens a square, right click flags one (red
    in it), and F2 starts again, every square raised once more."""
    desktop = s.settle("desktop")
    s.launch(s.tool("xd-mines"), "--seed", "7")
    s.wait_window(title="Minefield", state="activated")
    start, box = wait_change(s, desktop, (0, 0, WIDTH, HEIGHT - 40), "Minefield's window", "window")
    # The board's sunken edge is white along its bottom and right (the
    # window's frame isn't): the squares, 16 px, end where it starts.
    white = (255, 255, 255)
    middle_x = box[0] + box[2] // 2
    edge_y = next(y for y in range(box[1] + box[3], box[1], -1)
                  if near(start.pixel(middle_x, y), white, 8))
    while near(start.pixel(middle_x, edge_y - 1), white, 8):
        edge_y -= 1
    row_y = edge_y - 16 * 4 - 8  # the middle row's middle
    edge_x = next(x for x in range(box[0] + box[2], box[0], -1)
                  if near(start.pixel(x, row_y), white, 8))
    while near(start.pixel(edge_x - 1, row_y), white, 8):
        edge_x -= 1
    centre_x = edge_x - 16 * 4 - 8  # the middle column's middle

    def raised(shot, x, y):
        """A covered square: white along its top left edge."""
        return near(shot.pixel(x - 6, y - 7), (255, 255, 255), 30)

    def red_in(shot, x, y):
        return any(near(shot.pixel(x + dx, y + dy), (255, 0, 0), 40)
                   for dx in range(-5, 6) for dy in range(-6, 3))

    s.check(raised(start, centre_x, row_y), "the squares start covered")
    s.inject("move", centre_x, row_y, "click")
    s.wait(lambda: not raised(s.shot("opened"), centre_x, row_y), "the square to open")
    s.log("ok: a click opened the square")
    flag_x = centre_x - 16 * 4  # the left column, far from the opened area
    s.inject("move", flag_x, row_y, "click", "right")
    s.wait(lambda: red_in(s.shot("flagged"), flag_x, row_y), "a red flag")
    s.log("ok: a right click flagged a square")
    s.inject("key", "F2")
    s.wait(lambda: (lambda shot: raised(shot, centre_x, row_y) and not red_in(shot, flag_x, row_y))(
        s.shot("new-game")), "a new game")
    s.log("ok: F2 started a new game")


VBOX_SCRIPT = """until {go}
enter @
wait 400
move @
wait 400
move @
wait 400
drop @
file hello.txt Hello from the host
dir pics
file pics/b.txt Bee
"""


def vbox_dnd_setup(s):
    """A scripted VirtualBox host for xd-vboxdnd (the compositor starts it
    with $XD_VBOXDND_FAKE), dropping into a scratch TMPDIR."""
    s.vbox_go = os.path.join(s.base, "vbox-go")
    script = os.path.join(s.base, "vbox.script")
    with open(script, "w") as f:
        f.write(VBOX_SCRIPT.format(go=s.vbox_go))
    s.vbox_log = script + ".log"
    tmp = os.path.join(s.base, "tmp")
    os.makedirs(tmp, exist_ok=True)
    os.environ["TMPDIR"] = tmp
    os.environ["XD_VBOXDND_FAKE"] = script


@scenario(setup=vbox_dnd_setup)
def vbox_drag_into_files(s):
    """VirtualBox's host-to-guest drag and drop (xd-vboxdnd, against a
    scripted host): files dragged from the host onto an Explorer window are
    received, then dropped there, and Explorer copies them in."""
    x, y, _ = open_documents(s)
    # An empty part of the folder view, under Letters and Notes.txt.
    point = (x + LETTERS[0] + 40, y + LETTERS[1] + 160)
    with open(s.vbox_go, "w") as f:
        f.write(f"{point[0]} {point[1]}\n")
    documents = os.path.join(s.home, "Documents")
    hello = os.path.join(documents, "hello.txt")
    bee = os.path.join(documents, "pics", "b.txt")

    def arrived():
        try:
            with open(hello) as a, open(bee) as b:
                return a.read() == "Hello from the host" and b.read() == "Bee"
        except OSError:
            return False

    s.wait(arrived, "the dropped files in Documents", timeout=30)
    s.log("ok: the host's file and folder were dropped into Documents")
    with open(s.vbox_log) as f:
        log = f.read().splitlines()
    s.check(log[1:2] == ["ack 1"] and "enter refused" not in log,
            f"the drag's entry accepted, or the host (Qt) sends nothing more ({log})")
    s.check("ack 1" in log[2:], f"a move answered with Copy ({log})")
    s.check("request text/uri-list" in log, f"the files asked for at the drop ({log})")
    s.check(not any(line.startswith(("error", "guest error")) for line in log),
            f"no errors either side ({log})")
    s.log("ok: the guest answered the host as it expects")


VBOX_OUT_SCRIPT = """until {go}
wait 1500
pending
wait 300
dropped text/uri-list
"""


def vbox_out_setup(s):
    """A scripted VirtualBox host that, once told, asks whether a guest
    drag is going on, then takes it as dropped on the host."""
    s.vbox_go = os.path.join(s.base, "vbox-go")
    script = os.path.join(s.base, "vbox-out.script")
    with open(script, "w") as f:
        f.write(VBOX_OUT_SCRIPT.format(go=s.vbox_go))
    s.vbox_log = script + ".log"
    os.environ["XD_VBOXDND_FAKE"] = script


@scenario(setup=vbox_out_setup)
def vbox_drag_out_of_files(s):
    """VirtualBox's guest-to-host drag and drop (xd-vboxdnd, against a
    scripted host): a file dragged out of Explorer is reported to the host
    when it asks, and sent to it when dropped there; the drag here then
    ends without moving anything."""
    x, y, _ = open_documents(s)
    notes = (x + LETTERS[0] + 80, y + LETTERS[1])  # Notes.txt, next to Letters
    # The host asks 1.5 s after this, while the button is still held (the
    # drag ends when xd-inject lets go of it).
    with open(s.vbox_go, "w") as f:
        f.write("0 0\n")
    s.inject("move", *notes, "sleep", 200, "down", "sleep", 200,
             *[step for d in (10, 30, 60, 90) for step in ("move", notes[0] + d, notes[1] + d,
                                                           "sleep", 100)],
             "sleep", 4000)

    def log():
        try:
            with open(s.vbox_log) as f:
                return f.read().splitlines()
        except OSError:
            return []

    s.wait(lambda: "send file-data Shopping list\\n" in log(), "Notes.txt sent to the host",
           timeout=30)
    lines = log()
    s.check(any(line.startswith("pending 1 ") and "text/uri-list" in line for line in lines),
            f"the drag reported to the host, files first ({lines})")
    s.check("send file Notes.txt 14" in lines, f"the file sent whole ({lines})")
    s.check(not any(line.startswith(("error", "guest error")) for line in lines),
            f"no errors either side ({lines})")
    s.log("ok: the drag went to the host, its file with it")
    notes_path = os.path.join(s.home, "Documents", "Notes.txt")
    s.check(os.path.exists(notes_path), "Notes.txt is still in Documents (a copy, not a move)")
    s.log("ok: nothing moved here")


VBOX_STALL_SCRIPT = """until {go}
enter @
wait 300
move @
until {go}2
enter @
wait 300
move @
wait 60000
"""


def vbox_stall_setup(s):
    """A scripted VirtualBox host whose drag enters, then goes quiet."""
    s.vbox_go = os.path.join(s.base, "vbox-go")
    script = os.path.join(s.base, "vbox-stall.script")
    with open(script, "w") as f:
        f.write(VBOX_STALL_SCRIPT.format(go=s.vbox_go))
    os.environ["XD_VBOXDND_FAKE"] = script


@scenario(setup=vbox_stall_setup)
def vbox_drag_escape(s):
    """A VirtualBox host's drag that doesn't finish doesn't keep the
    pointer: Esc ends it, and a click selects again."""
    x, y, before = open_documents(s)
    point = (x + LETTERS[0] + 40, y + LETTERS[1] + 160)
    status = (x + 8, y + 570, 200, 20)  # the status bar's left part
    notes = (x + LETTERS[0] + 80, y + LETTERS[1])
    empty = (x + LETTERS[0] + 300, y + LETTERS[1] + 250)
    with open(s.vbox_go, "w") as f:
        f.write(f"{point[0]} {point[1]}\n")
    time.sleep(1.5)  # entered and moved: the drag holds the pointer
    s.inject("key", "Escape", "sleep", 300, "move", *notes, "click")
    s.wait(lambda: s.shot("selected").differs(before, status) > 0.01,
           "a click to select Notes.txt after Esc")
    s.log("ok: Esc ended the host's drag; clicks select again")
    # Unselected again (a click on nothing), then the host drags in again.
    s.inject("move", *empty, "click", "sleep", 300)
    unselected = s.settle("unselected")
    with open(s.vbox_go + "2", "w") as f:
        f.write(f"{point[0]} {point[1]}\n")
    time.sleep(1.5)
    s.inject("move", *notes, "click", "sleep", 500)
    s.check(s.shot("swallowed").differs(unselected, status) < 0.01,
            "the click that ends the host's drag goes no further")
    s.inject("click")
    s.wait(lambda: s.shot("selected-again").differs(unselected, status) > 0.01,
           "the next click to select Notes.txt")
    s.log("ok: a click ended the host's drag; the next one selects")


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
    dialog saves its options on OK (Marquee's text, 3D Pipes' Multiple,
    3D Maze's Pixelated, 3D Flower Box's shape)."""
    empty = s.settle("desktop")
    s.launch("xd-desk", "--page", "2")
    s.wait_window(title="Display Properties", state="activated")
    dialog = s.settle("screen-saver-tab").changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(dialog is not None, f"Display Properties is up at {dialog}")
    # Focus starts on the tabs; the saver list is next. (None) is chosen.
    s.inject("key", "Tab", "sleep", 200)
    savers = ["3D Flower Box", "3D Maze", "3D Pipes", "3D Text", "Beziers", "Blank Screen", "Flying Penguins",
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

    # 3D Pipes (after (None), 3D Flower Box and 3D Maze): Single to
    # Multiple with the arrow keys.
    s.wait_window(title="Display Properties", state="activated")
    s.inject("key", "shift+Tab", "sleep", 200, "key", "Home", "sleep", 200, "key", "Down",
             "sleep", 200, "key", "Down", "sleep", 200, "key", "Down",
             "sleep", 300, "key", "Tab", "sleep", 200, "key", "space")
    s.wait_window(title="3D Pipes Setup", state="activated")
    s.settle("pipes-setup")
    s.inject("key", "Down", "sleep", 200, "key", "Return")
    s.wait_window(title="3D Pipes Setup", gone=True)
    s.wait(lambda: s.ini().get("ScreenSaver.Pipes", "Pipes", fallback="") == "multiple",
           "desktop.ini to have Multiple pipes")
    s.log("ok: 3D Pipes Setup saved Multiple")

    # 3D Maze: Pixelated off (Speed, Maze size, then the check box).
    s.wait_window(title="Display Properties", state="activated")
    s.inject("key", "shift+Tab", "sleep", 200, "key", "Up", "sleep", 300, "key", "Tab",
             "sleep", 200, "key", "space")
    s.wait_window(title="3D Maze Setup", state="activated")
    s.settle("maze-setup")
    s.inject("key", "Tab", "sleep", 200, "key", "Tab", "sleep", 200, "key", "space",
             "sleep", 200, "key", "Return")
    s.wait_window(title="3D Maze Setup", gone=True)
    s.wait(lambda: s.ini().get("ScreenSaver.Maze", "Pixelated", fallback="") == "false",
           "desktop.ini to have the maze smooth")
    s.log("ok: 3D Maze Setup saved Pixelated off")

    # 3D Flower Box: the Shape drop-down, from Cube to Tetrahedron.
    s.wait_window(title="Display Properties", state="activated")
    s.inject("key", "shift+Tab", "sleep", 200, "key", "Up", "sleep", 300, "key", "Tab",
             "sleep", 200, "key", "space")
    s.wait_window(title="3D Flower Box Setup", state="activated")
    s.settle("flower-setup")
    s.inject("key", "Down", "sleep", 200, "key", "Return")
    s.wait_window(title="3D Flower Box Setup", gone=True)
    s.wait(lambda: s.ini().get("ScreenSaver.FlowerBox", "Shape", fallback="") == "tetrahedron",
           "desktop.ini to have the tetrahedron")
    s.log("ok: 3D Flower Box Setup saved the shape")


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
    s.launch("xd-desk", "--page", "5")
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
    s.launch("xd-desk", "--page", "5")
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


@scenario()
def sandbox_globals(s):
    """Sandboxed programs (Flatpak's, through security-context-v1) get
    none of the protocols that reach past their own windows; programs on
    the desktop keep them (screenshot tools, clipboard managers)."""

    def globals(*args):
        r = subprocess.run([s.tool("xd-globals"), *args], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=20)  # fmt: skip
        if r.returncode != 0:
            raise Failure("xd-globals: " + r.stderr.decode().strip())
        return set(r.stdout.decode().split())

    privileged = {
        "zwlr_screencopy_manager_v1", "zwlr_export_dmabuf_manager_v1",
        "zwlr_data_control_manager_v1", "ext_data_control_manager_v1",
        "zwlr_foreign_toplevel_manager_v1", "zwlr_output_manager_v1",
        "zwlr_output_power_manager_v1", "zwlr_gamma_control_manager_v1",
        "zwlr_layer_shell_v1", "ext_session_lock_manager_v1",
    }  # fmt: skip
    plain = globals()
    s.check(privileged <= plain, f"a program gets them all (missing {privileged - plain})")
    boxed = globals("--sandboxed")
    s.check({"wl_compositor", "xdg_wm_base", "wl_seat"} <= boxed,
            f"a sandboxed program gets the ordinary ones ({sorted(boxed)})")  # fmt: skip
    leaked = (privileged | {"zwp_virtual_keyboard_manager_v1", "ext_foreign_toplevel_list_v1",
                            "xd_window_arranger_v1", "zwp_input_method_manager_v2"}) & boxed
    s.check(not leaked, f"a sandboxed program gets none of them ({sorted(leaked)})")
    s.log(f"ok: {len(plain)} globals, {len(boxed)} sandboxed")


@scenario(AERO_INI, requires=("dbusmock",))
def clock_flyout(s):
    """The unified taskbar's clock opens Windows 7's flyout above it (the
    month and a clock, at noon here: XD_CLOCK_FIXED); Esc closes it, and
    "Change date and time settings..." opens Date and Time Properties."""
    mock_timedate(s)
    clock = (WIDTH - 40, HEIGHT - 20)
    away = (WIDTH // 2, HEIGHT // 2)
    above = (0, 0, WIDTH, HEIGHT - 40)
    empty = s.settle("desktop")
    s.inject("move", *clock, "click", "move", *away)
    shown = s.settle("flyout")
    box = shown.changed_box(empty, above)
    s.check(box is not None and 340 <= box[2] <= 360 and box[1] + box[3] >= HEIGHT - 44,
            f"the flyout above the clock ({box})")  # fmt: skip
    s.inject("key", "Escape")
    s.wait(lambda: s.shot("closed").changed_box(empty, above) is None, "Esc to close it")
    s.inject("move", *clock, "click", "move", *away)
    s.settle("again")
    # The link, centred in the band along the flyout's foot.
    s.inject("move", box[0] + box[2] // 2, box[1] + box[3] - 18, "click")
    s.wait_window(title="Date and Time Properties", state="activated")
    s.log("ok: the link opened Date and Time Properties")


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

    # Location: turn it on, deny Maps. A loaded CI runner has dropped the
    # Ctrl+Tab; send it again until the page has changed.
    general = s.settle("general")
    for _ in range(3):
        s.inject("key", "ctrl+Tab", "sleep", 300)
        if s.settle("location").differs(general, (0, 0, WIDTH, HEIGHT - 40)) > 0.01:
            break
    else:
        raise Failure("Ctrl+Tab didn't open the Location tab")
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


def disk_cleanup_setup(s):
    """Before the compositor starts: the scratch Flatpak installations with
    the test app uninstalled (its runtime left unused), a scratch system
    root with a package cache, archived journal files and a crash dump,
    stand-ins for pacman, pacman-conf and journalctl that log what they're
    asked, xd-cleanmgr-helper run directly (no pkexec), and no temporary
    folders (the real ones are the host's)."""
    scratch_flatpak(s)
    if s.flatpak is not None:
        subprocess.run([s.flatpak, "--user", "uninstall", "-y", "--noninteractive", FLATPAK_APP],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # fmt: skip
    root = os.path.join(s.base, "sysroot")
    tools = os.path.join(s.base, "systools")
    calls = os.path.join(s.base, "syscalls")
    os.makedirs(tools)
    old = time.time() - 30 * 24 * 3600
    for rel in ("var/cache/pacman/pkg/foo-0.9-1-x86_64.pkg.tar.zst",
                "var/cache/pacman/pkg/foo-1.0-1-x86_64.pkg.tar.zst",
                "var/log/journal/m/system@0001-0002.journal",
                "var/lib/systemd/coredump/core.prog.1000.abc.zst"):  # fmt: skip
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"x" * 200000)
        os.utime(path, (old, old))
    scripts = {
        "pacman": f"case \"$1\" in -Q) echo 'foo 1.0-1';; *) echo \"pacman $*\" >>{calls};; esac\n",
        "pacman-conf": "echo /var/cache/pacman/pkg/\n",
        "journalctl": f"echo \"journalctl $*\" >>{calls}\n",
    }
    for name, body in scripts.items():
        path = os.path.join(tools, name)
        with open(path, "w") as f:
            f.write("#!/bin/sh\n" + body)
        os.chmod(path, 0o755)
    os.environ.update({"XD_CLEANMGR_ROOT": root, "XD_CLEANMGR_TOOLS": tools,
                       "XD_CLEANMGR_HELPER": s.tool("xd-cleanmgr-helper"),
                       "XD_CLEANUP_TEMP_DIRS": ""})  # fmt: skip


@scenario(LUNA_INI, setup=disk_cleanup_setup)
def disk_cleanup_linux(s):
    """Disk Cleanup on its own (xd-files --disk-cleanup, the Start menu's):
    the graphics shader, developer tools' and programs' caches ticked go;
    the unused Flatpak runtime goes (ticked to start with); Clean up system
    files adds the package cache, old system logs and crash dumps, cleaned
    by xd-cleanmgr-helper: pacman -Sc, journalctl --vacuum-time, the dump
    deleted."""
    cache = os.environ["XDG_CACHE_HOME"]
    seeded = {
        "thumbnails": os.path.join(cache, "thumbnails", "normal", "t.png"),
        "shaders": os.path.join(cache, "mesa_shader_cache", "ab", "cd"),
        "developer": os.path.join(cache, "pip", "http", "w"),
        "programs": os.path.join(cache, "some-program", "data"),
    }
    for path in seeded.values():
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(b"x" * 100000)
    calls = os.path.join(s.base, "syscalls")
    dump = os.path.join(os.environ["XD_CLEANMGR_ROOT"], "var/lib/systemd/coredump",
                        "core.prog.1000.abc.zst")  # fmt: skip
    runtime = (os.path.join(os.environ["FLATPAK_USER_DIR"], "runtime", "org.xd.Test.Platform")
               if s.flatpak is not None else None)  # fmt: skip

    def cleanup_window():
        return next((w for w in s.windows() if w["title"].startswith("Disk Cleanup for")), None)

    s.launch("xd-files", "--disk-cleanup")
    s.wait(lambda: cleanup_window() is not None or s.window(title="Disk Cleanup : Drive Selection"),
           "Disk Cleanup")  # fmt: skip
    if s.window(title="Disk Cleanup : Drive Selection") is not None:
        s.inject("key", "Return")  # the first drive: the root file system
    s.wait(cleanup_window, "Disk Cleanup's window")
    s.settle("scanned")
    time.sleep(1)
    listed = s.settle("disk cleanup")
    # The rows: Recycle Bin (selected), Thumbnails, shader, developer
    # tools' and programs' caches, [unused Flatpak runtimes]: tick the
    # three caches.
    s.inject("key", "Down", "sleep", 150, "key", "Down", "key", "space", "sleep", 150,
             "key", "Down", "key", "space", "sleep", 150, "key", "Down", "key", "space")  # fmt: skip
    s.settle("caches ticked")
    s.inject("key", "alt+s")
    time.sleep(1.5)
    added = s.settle("system files")
    s.check(added.differs(listed, (0, 0, WIDTH, HEIGHT - 40)) > 0.001,
            "Clean up system files added to the list")  # fmt: skip
    # Back to the list (still on programs' caches): past the Flatpak row
    # and the package cache (ticked), tick the old system logs.
    downs = ["key", "Down"] * (3 if runtime is not None else 2)
    s.inject("key", "alt+f", "sleep", 200, *downs, "key", "space", "sleep", 200, "key", "Return",
             "sleep", 800)  # fmt: skip
    s.settle("are you sure")
    s.inject("key", "Return")

    def cleaned():
        log = open(calls).read() if os.path.exists(calls) else ""
        return (not os.path.exists(seeded["shaders"]) and not os.path.exists(seeded["developer"])
                and not os.path.exists(seeded["programs"]) and not os.path.exists(dump)
                and "pacman -Sc --noconfirm" in log and "journalctl --vacuum-time=2weeks" in log
                and (runtime is None or not os.path.exists(runtime)))  # fmt: skip

    s.wait(cleaned, "Disk Cleanup to clean everything ticked", timeout=40)
    s.log("ok: caches, the package cache, old logs and the crash dump cleaned")
    if runtime is not None:
        s.log("ok: the unused Flatpak runtime was uninstalled")
    s.check(not os.path.exists(seeded["thumbnails"]), "Thumbnails (ticked to start with) went")
    s.check(os.path.isdir(os.path.join(cache, "mesa_shader_cache")),
            "the cache folders themselves stayed")  # fmt: skip
    s.wait(lambda: cleanup_window() is None, "Disk Cleanup to finish")


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
    s.launch("xd-desk", "--page", "6")
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
    # desktop.ini changed, so GTK programs reload their theme: a key sent
    # meanwhile can be lost.
    s.settle("reloaded")
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


def fake_snapper_volume(s, name, snapshots):
    """A scratch "subvolume" in the home, with Snapper's snapshot trees
    in it (.snapshots/N/snapshot): `snapshots` maps each number to its
    date and its files (relative path: (text, mtime)). Returns the
    subvolume and the dbusmock parameters naming it."""
    volume = os.path.join(s.home, name)
    for number, (date, files) in snapshots.items():
        tree = os.path.join(volume, ".snapshots", str(number), "snapshot")
        for rel, (text, mtime) in files.items():
            path = os.path.join(tree, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(text)
            os.utime(path, (mtime, mtime))
    params = {
        "configs": {name.lower(): volume},
        "snapshots": json.dumps(
            {name.lower(): [[n, date, "timeline"] for n, (date, _) in snapshots.items()]}),
    }
    return volume, params


def start_snapper_mock(s, params):
    from dbusmock import DBusTestCase

    DBusTestCase.start_system_bus()
    mock = os.path.join(SOURCE_ROOT, "tests", "dbusmock", "snapper.py")
    proc, _ = DBusTestCase.spawn_server_template(
        mock, params, stdout=subprocess.DEVNULL, system_bus=True
    )
    s.children.append(proc)


@scenario(desktop_ini=LUNA_INI, requires=("dbusmock",))
def files_previous_versions(s):
    """A file's Properties (XP's style), on a dbusmock Snapper whose
    snapshots are scratch trees: the Previous Versions tab lists the
    versions where the file changed (snapshots 1 and 2 hold the same copy),
    and Restore, after its question, puts the newest one back; a file in
    none of the snapshots has the tab, saying there are no versions."""
    from dbusmock import DBusTestCase

    now = int(time.time())
    volume, params = fake_snapper_volume(s, "Work", {
        1: (now - 3 * 86400, {"report.txt": ("first draft", now - 4 * 86400)}),
        2: (now - 2 * 86400, {"report.txt": ("first draft", now - 4 * 86400)}),
        3: (now - 86400, {"report.txt": ("second draft", now - 86400 - 60)}),
    })  # fmt: skip
    report = os.path.join(volume, "report.txt")
    with open(report, "w") as f:
        f.write("current")
    try:
        start_snapper_mock(s, params)
        env = {"XD_SNAPSHOT_ROOTS": "snapper"}
        s.launch("xd-files", "--properties", report, env=env)
        s.wait_window(title="report.txt Properties", state="activated")
        before = s.settle("general")
        # The tab comes last, once Snapper has answered.
        time.sleep(1.5)
        s.inject("key", "ctrl+shift+Tab", "sleep", 500)
        shot = s.settle("previous versions")
        s.check(shot.changed_box(before, (0, 0, WIDTH, HEIGHT - 40)) is not None,
                "a Previous Versions tab")  # fmt: skip
        # The list, its first (newest) version: snapshot 3's.
        s.inject("key", "alt+f", "sleep", 300, "key", "Home", "sleep", 300)
        s.settle("newest selected")
        s.inject("key", "alt+r", "sleep", 600)
        s.settle("restore?")
        s.inject("key", "alt+y", "sleep", 800)
        s.wait(lambda: file_text(report) == "second draft", "the restored file")
        s.settle("restored")
        s.inject("key", "Return", "sleep", 400)  # "The file has been successfully restored."
        s.log("ok: Restore put snapshot 3's copy back")
        s.inject("key", "Escape")
        s.wait_window(title="report.txt Properties", gone=True)
        names = set(os.listdir(volume))
        s.check(names == {"report.txt", ".snapshots"}, f"nothing else written ({names})")

        # A file in none of the snapshots: the tab, saying there are none.
        todo = os.path.join(volume, "todo.txt")
        with open(todo, "w") as f:
            f.write("new")
        s.launch("xd-files", "--properties", todo, env=env)
        s.wait_window(title="todo.txt Properties", state="activated")
        before = s.settle("todo general")
        time.sleep(1.5)
        s.inject("key", "ctrl+shift+Tab", "sleep", 500)
        shot = s.settle("no previous versions")
        s.check(shot.changed_box(before, (0, 0, WIDTH, HEIGHT - 40)) is not None,
                "a Previous Versions tab for a file without versions")  # fmt: skip
        s.inject("key", "Escape")
        s.wait_window(title="todo.txt Properties", gone=True)
    finally:
        DBusTestCase.tearDownClass()


@scenario(desktop_ini=AERO_INI, requires=("dbusmock",))
def files_previous_versions_seven(s):
    """7's "Restore previous versions" on a folder's context menu (a
    dbusmock Snapper): its Properties open on the Previous Versions tab,
    and Restore... copies the snapshot's files back into the folder,
    leaving the file the snapshot didn't have; Copy... copies the
    snapshot's folder into another."""
    from dbusmock import DBusTestCase

    now = int(time.time())
    volume, params = fake_snapper_volume(s, "Work", {
        4: (now - 86400, {"Project/plan.txt": ("old plan", now - 2 * 86400),
                          "Project/notes/a.txt": ("note a", now - 2 * 86400)}),
    })  # fmt: skip
    project = os.path.join(volume, "Project")
    os.makedirs(project)
    with open(os.path.join(project, "plan.txt"), "w") as f:
        f.write("new plan")
    with open(os.path.join(project, "extra.txt"), "w") as f:
        f.write("kept")
    try:
        start_snapper_mock(s, params)
        env = {"XD_SNAPSHOT_ROOTS": "snapper"}
        s.launch("xd-files", volume, env=env)
        s.wait_window(app_id="org.xeondesktop.Files", state="activated")
        s.settle("work folder")
        # Project is the only item shown (.snapshots is hidden).
        s.inject("key", "Home", "sleep", 300, "key", "Menu", "sleep", 500)
        s.settle("context menu")
        s.inject("key", "v")
        # Over its window: xd-windows doesn't list dialogs, but Work loses
        # the focus.
        s.wait(lambda: "activated" not in s.window(title="Work")["states"],
               "Project Properties")  # fmt: skip
        time.sleep(1.5)
        s.settle("previous versions")
        s.inject("key", "alt+f", "sleep", 300, "key", "Home", "sleep", 300, "key", "alt+r",
                 "sleep", 600)  # fmt: skip
        s.settle("restore?")
        s.inject("key", "alt+y", "sleep", 800)
        s.wait(lambda: file_text(os.path.join(project, "plan.txt")) == "old plan",
               "the restored folder")  # fmt: skip
        s.check(file_text(os.path.join(project, "notes", "a.txt")) == "note a",
                "the snapshot's subfolder came back")  # fmt: skip
        s.check(file_text(os.path.join(project, "extra.txt")) == "kept",
                "what the snapshot didn't have stays")  # fmt: skip
        s.settle("restored")
        s.inject("key", "Return", "sleep", 400)
        s.log("ok: Restore... copied snapshot 4's files back into the folder")

        # Copy...: into another folder, chosen in the folder box.
        copies = os.path.join(s.home, "Copies")
        os.makedirs(copies)
        s.inject("key", "alt+f", "sleep", 300, "key", "Home", "sleep", 300, "key", "alt+c",
                 "sleep", 1000, "key", "ctrl+l", "sleep", 300, "type", copies + "/",
                 "sleep", 800)  # fmt: skip
        s.settle("copy to")
        s.inject("key", "alt+c")
        copied = os.path.join(copies, "Project", "plan.txt")
        s.wait(lambda: file_text(copied) == "old plan", "the copied version")
        s.check(not os.path.exists(os.path.join(copies, "Project", "extra.txt")),
                "the snapshot's copy, not the folder as it is")  # fmt: skip
        s.log("ok: Copy... copied snapshot 4's folder")
    finally:
        DBusTestCase.tearDownClass()


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
    a two-finger swipe and a hold) reach it with their fingers and scale;
    a three-finger swipe is the desktop's (touchpad_desktop_swipes)."""
    log, (x, y, w, h) = input_window(s)
    s.inject("move", x + 100, y + 100, "pinch", "begin", 2, "pinch", "update", 0, 0, 1.5, 10,
             "pinch", "end", "swipe", "begin", 2, "swipe", "update", 30, 0, "swipe", "end",
             "rest", "begin", 2, "rest", "end", "cancel")  # fmt: skip
    wait_logged(s, log, "hold end", "the hold to end")
    got = logged(log)
    s.check("pinch begin 2" in got and "pinch update scale 1.50 rotation 10.0" in got
            and "pinch end" in got, "the pinch reached the program, scale 1.5")  # fmt: skip
    s.check("swipe begin 2" in got and "swipe update 30.0 0.0" in got and "swipe end" in got,
            "a two-finger swipe too")  # fmt: skip
    s.check("hold begin 2" in got and "hold end cancelled" in got, "and a cancelled hold")


@scenario(CLASSIC_INI + "\n[Touchpad]\nGestures=false\n")
def touchpad_swipes_to_programs(s):
    """With the desktop's gestures off ([Touchpad] Gestures), a
    three-finger swipe goes to the program under the pointer too."""
    log, (x, y, w, h) = input_window(s)
    s.inject("move", x + 100, y + 100, "swipe", "begin", 3, "swipe", "update", 0, 80,
             "swipe", "end")  # fmt: skip
    wait_logged(s, log, "swipe end", "the swipe to end")
    s.check("swipe begin 3" in logged(log), "the program got the three-finger swipe")
    s.check("minimized" not in s.window(title="Input test")["states"], "and nothing minimized")


@scenario()
def touchpad_desktop_swipes(s):
    """Three-finger swipes are the desktop's, as on Windows' precision
    touchpads: down minimizes the windows showing, up brings them back,
    sideways switches windows as Alt+Tab does; the program under the
    fingers sees none of it."""
    log_a, (x, y, w, h) = input_window(s, title="Input A")
    log_b, _ = input_window(s, title="Input B")

    def states(title):
        return s.window(title=title)["states"]

    def swipe(dx, dy):
        steps = []
        for _ in range(4):
            steps += ["swipe", "update", dx / 4, dy / 4]
        s.inject("swipe", "begin", 3, *steps, "swipe", "end")

    s.inject("move", x + 100, y + 100)
    swipe(0, 200)
    s.wait(lambda: "minimized" in states("Input A") and "minimized" in states("Input B"),
           "a swipe down to minimize both windows")  # fmt: skip
    s.log("ok: three fingers down showed the desktop")
    swipe(0, -200)
    s.wait(lambda: "minimized" not in states("Input A") and "minimized" not in states("Input B"),
           "a swipe up to bring them back")  # fmt: skip
    s.log("ok: three fingers up brought the windows back")
    front = "Input A" if "activated" in states("Input A") else "Input B"
    other = "Input B" if front == "Input A" else "Input A"
    swipe(150, 0)
    s.wait_window(title=other, state="activated")
    s.log(f"ok: three fingers sideways switched from {front} to {other}")
    got = logged(log_a) + logged(log_b)
    s.check(not any(line.startswith("swipe") for line in got),
            "the programs saw none of the swipes")  # fmt: skip


@scenario()
def touchpad_settings_tab(s):
    """Mouse Properties' Touchpad tab (shown here without a touchpad by
    XD_SHOW_TOUCHPAD): Tap to click turned off and reverse scrolling on,
    by their access keys, reach [Touchpad] on Apply."""
    s.launch(s.tool("xd-mouse"), env={"XD_SHOW_TOUCHPAD": "1", "GSETTINGS_BACKEND": "memory"})
    s.wait_window(title="Mouse Properties", state="activated")
    s.settle("mouse")
    s.inject("key", "alt+c", "sleep", 300)
    s.settle("touchpad tab")
    s.inject("key", "alt+t", "sleep", 200, "key", "alt+v", "sleep", 200, "key", "alt+a")

    def saved():
        ini = s.ini()
        return (ini.get("Touchpad", "TapToClick", fallback="") == "false"
                and ini.get("Touchpad", "NaturalScrolling", fallback="") == "true")  # fmt: skip

    s.wait(saved, "TapToClick=false and NaturalScrolling=true in [Touchpad]")
    s.check(s.ini().get("Touchpad", "Scrolling", fallback="") == "twofinger",
            "the rest as it was (two-finger scrolling)")  # fmt: skip
    s.log("ok: the Touchpad tab saved [Touchpad]")


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
THEME_PKG_BUTTON = (30, 60, 230)


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
    png("button.png", 10, 10, THEME_PKG_BUTTON)
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
    n, buttons = color_box(shot, THEME_PKG_BUTTON)
    s.check(n > 3 * 60 * 15 and buttons[1] > cap[3] and cap[0] <= buttons[0]
            and buttons[2] <= cap[2], f"the push buttons are the package's ({n} pixels at {buttons})")  # fmt: skip


# A package styling the XP taskbar (our own flat colours again): the bar,
# the Start button's three states and the task buttons' six.
PKG_BAR = (40, 170, 90)
PKG_START = ((150, 20, 20), (170, 20, 20), (190, 20, 20))
PKG_TASKS = [(20, 20, 100 + 20 * i) for i in range(6)]  # Normal ... HotChecked


def write_taskbar_package(s):
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    root = os.path.join(s.base, "barpkg")
    os.makedirs(os.path.join(root, "images"))
    os.makedirs(os.path.join(root, "styles"))

    def strip(name, w, frames):
        """A vertical strip of solid frames, w x w each."""
        pb = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, w, w * len(frames))
        for i, rgb in enumerate(frames):
            pb.new_subpixbuf(0, i * w, w, w).fill(
                (rgb[0] << 24) | (rgb[1] << 16) | (rgb[2] << 8) | 0xFF)  # fmt: skip
        pb.savev(os.path.join(root, "images", name), "png", [], [])

    strip("bar.png", 4, [PKG_BAR])
    strip("start.png", 8, PKG_START)
    strip("task.png", 8, PKG_TASKS)
    with open(os.path.join(root, "theme.xml"), "w") as f:
        f.write('<xdtheme version="1" name="UI Bar Test" base="luna">\n'
                '  <variant id="plain" name="Plain" size="normal" style="styles/plain.xml"/>\n'
                "</xdtheme>\n")  # fmt: skip
    with open(os.path.join(root, "styles", "plain.xml"), "w") as f:
        f.write("""<style>
  <class name="TaskBar">
    <part name="BackgroundBottom" ImageFile="images/bar.png"/>
  </class>
  <class name="Start::Button">
    <part ImageFile="images/start.png" ImageCount="3" ImageLayout="Vertical"
          SizingMargins="0 0 0 0" TextColor="255 255 255"/>
  </class>
  <class name="TaskBand::Toolbar">
    <part TextColor="255 255 255"/>
    <part name="Button" ImageFile="images/task.png" ImageCount="6" ImageLayout="Vertical"/>
  </class>
</style>
""")
    os.environ["XD_THEME_PACKAGE"] = root
    os.environ["XD_UXTHEME_NAMES"] = os.path.join(SOURCE_ROOT, "data", "theme", "uxtheme.ini")


@scenario(desktop_ini=LUNA_INI, setup=write_taskbar_package)
def theme_package_taskbar(s):
    """With $XD_THEME_PACKAGE set, the XP taskbar, its Start button (and its
    hot and pressed states) and the active window's task button come from
    the package's images."""
    s.launch("xd-run")
    s.wait_window(title="Run", state="activated")
    shot = s.settle("bar")
    n, bar = color_box(shot, PKG_BAR)
    s.check(n > WIDTH * 10 and bar[0] <= 200 and bar[2] >= WIDTH - 200 and bar[3] == HEIGHT - 1,
            f"the taskbar is the package's ({n} pixels at {bar})")  # fmt: skip
    n, start = color_box(shot, PKG_START[0])
    s.check(n > 40 * 10 and start[0] == 0 and start[3] == HEIGHT - 1,
            f"the Start button is the package's ({n} pixels at {start})")  # fmt: skip
    # The active window's button is Checked (5), not Normal (1).
    n, task = color_box(shot, PKG_TASKS[4])
    s.check(n > 60 * 10 and task[0] > start[2] and task[1] > HEIGHT - 40,
            f"the active window's task button is the package's Checked one ({n} pixels at {task})")  # fmt: skip
    s.check(color_box(shot, PKG_TASKS[0])[0] == 0, "no task button is drawn Normal")

    # Hot, then pressed (its menu open).
    s.inject("move", start[0] + 4, start[1] + 4, "sleep", 300)
    hot = s.settle("start-hot")
    s.check(color_box(hot, PKG_START[1])[0] > 40 * 10, "the hot Start button is the package's")
    s.inject("click", "sleep", 500)
    pressed = s.settle("start-pressed")
    s.check(color_box(pressed, PKG_START[2])[0] > 40 * 10,
            "the Start button is pressed while its menu is open")  # fmt: skip
    s.inject("key", "Escape")
    s.log("ok: the taskbar, Start button and task button are the package's")


# A package with 7's Alt+Tab class (AltTab, its deduced part names): the box
# and the selected item's frame in our own flat colours.
PKG_ALTTAB_BOX = (210, 180, 40)
PKG_ALTTAB_SELECTED = (40, 90, 210)


def write_alttab_package(s):
    import gi

    gi.require_version("GdkPixbuf", "2.0")
    from gi.repository import GdkPixbuf

    root = os.path.join(s.base, "alttabpkg")
    os.makedirs(os.path.join(root, "images"))
    os.makedirs(os.path.join(root, "styles"))

    def png(name, rgb):
        pb = GdkPixbuf.Pixbuf.new(GdkPixbuf.Colorspace.RGB, True, 8, 8, 8)
        pb.fill((rgb[0] << 24) | (rgb[1] << 16) | (rgb[2] << 8) | 0xFF)
        pb.savev(os.path.join(root, "images", name), "png", [], [])

    png("box.png", PKG_ALTTAB_BOX)
    png("selected.png", PKG_ALTTAB_SELECTED)
    with open(os.path.join(root, "theme.xml"), "w") as f:
        f.write('<xdtheme version="1" name="UI Alt+Tab Test" base="aero">\n'
                '  <variant id="plain" name="Plain" size="normal" style="styles/plain.xml"/>\n'
                "</xdtheme>\n")  # fmt: skip
    with open(os.path.join(root, "styles", "plain.xml"), "w") as f:
        f.write("""<style>
  <class name="AltTab">
    <part ImageFile="images/box.png" SizingMargins="0 0 0 0"/>
    <part name="SelectedItem" ImageFile="images/selected.png" SizingMargins="0 0 0 0"/>
    <part name="Title" TextColor="0 0 0"/>
  </class>
</style>
""")
    os.environ["XD_THEME_PACKAGE"] = root
    os.environ["XD_UXTHEME_NAMES"] = os.path.join(SOURCE_ROOT, "data", "theme", "uxtheme.ini")


@scenario(desktop_ini=AERO_INI, setup=write_alttab_package)
def alt_tab_package(s):
    """While Alt is held after Tab, Alt+Tab's window shows the windows,
    drawn from a package's AltTab class: its box, and the selected
    window's frame; releasing Alt switches and closes it."""
    two_windows(s)
    before = s.settle("before")
    s.check(color_box(before, PKG_ALTTAB_BOX)[0] == 0, "no Alt+Tab window before Alt+Tab")
    # Alt held in one xd-inject (its keyboard goes with it), the screenshot
    # taken meanwhile.
    held = subprocess.Popen([s.tool("xd-inject"), "hold", "alt", "key", "Tab", "sleep", "4000",
                             "release", "alt"])  # fmt: skip
    time.sleep(1.5)
    shot = s.settle("switcher")
    n, box = color_box(shot, PKG_ALTTAB_BOX)
    s.check(n > 150 * 60, f"Alt+Tab's window is the package's box ({n} pixels at {box})")
    s.check(abs((box[0] + box[2]) / 2 - WIDTH / 2) < 20,
            f"Alt+Tab's window is centred ({box})")  # fmt: skip
    n, sel = color_box(shot, PKG_ALTTAB_SELECTED)
    s.check(n > 1000 and box[0] < sel[0] and sel[2] < box[2] and box[1] < sel[1] < box[3],
            f"the selected window has the package's frame, in the box ({n} pixels at {sel})")  # fmt: skip
    # Two windows: the second is selected, to the right of the first.
    s.check(sel[0] > (box[0] + box[2]) / 2 - 10, f"the second window is selected ({sel})")
    s.check(held.wait(timeout=30) == 0, "xd-inject held Alt and let go")
    s.wait_window(title="Control Panel", state="activated")
    after = s.settle("after")
    s.check(color_box(after, PKG_ALTTAB_BOX)[0] == 0, "releasing Alt closed Alt+Tab's window")
    s.log("ok: Alt+Tab's window is the package's, and closes with Alt")


# ---- the file chooser portal (xdg-desktop-portal-xeondesktop) ---------------------

PORTAL_NAME = "org.freedesktop.impl.portal.desktop.xeondesktop"
PORTAL_PATH = "/org/freedesktop/portal/desktop"
PORTAL_HANDLE = "/org/freedesktop/portal/desktop/request/1_1/t"


class PortalCall:
    """A FileChooser call to our backend, made straight to it on the session
    bus (as xdg-desktop-portal would), answered in a thread while the
    scenario drives the dialog."""

    def __init__(self, s, method, title, options, handle=PORTAL_HANDLE):
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio, GLib

        self.Gio = Gio
        self.handle = handle
        self.result = None
        self.error = None
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        args = GLib.Variant("(osssa{sv})", (handle, "org.example.Test", "", title, options))

        def run():
            try:
                self.result = self.bus.call_sync(
                    PORTAL_NAME, PORTAL_PATH, "org.freedesktop.impl.portal.FileChooser",
                    method, args, GLib.VariantType("(ua{sv})"), Gio.DBusCallFlags.NONE,
                    120000, None,
                ).unpack()  # fmt: skip
            except Exception as e:  # noqa: BLE001
                self.error = e

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()

    def close(self):
        self.bus.call_sync(
            PORTAL_NAME, self.handle, "org.freedesktop.impl.portal.Request", "Close", None,
            None, self.Gio.DBusCallFlags.NONE, 10000, None,
        )  # fmt: skip

    def wait(self, s, timeout=30):
        self.thread.join(timeout)
        if self.thread.is_alive():
            raise Failure("the portal call didn't return")
        if self.error is not None:
            raise Failure(f"the portal call failed: {self.error}")
        s.log(f"portal answered {self.result}")
        return self.result


def start_portal(s):
    """Starts our backend in the session and waits for its bus name."""
    import gi

    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib

    s.launch(s.tool("xdg-desktop-portal-xeondesktop"))
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def owned():
        return bus.call_sync(
            "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
            "NameHasOwner", GLib.Variant("(s)", (PORTAL_NAME,)), GLib.VariantType("(b)"),
            Gio.DBusCallFlags.NONE, 5000, None,
        ).unpack()[0]  # fmt: skip

    s.wait(owned, "the portal backend's bus name")


def text_filters():
    from gi.repository import GLib

    return GLib.Variant("a(sa(us))", [("Text Documents (*.txt)", [(0, "*.txt")]),
                                      ("All Files", [(0, "*")])])  # fmt: skip


def in_documents(s, **options):
    """Options starting in Documents (current_folder, a byte string with its
    NUL), plus `options`."""
    from gi.repository import GLib

    folder = os.path.join(s.home, "Documents").encode() + b"\0"
    return {"current_folder": GLib.Variant("ay", folder), **options}


def documents_uri(s, *names):
    return "file://" + urllib.parse.quote(os.path.join(s.home, "Documents", *names))


@scenario(LUNA_INI)
def portal_open(s):
    """The Open dialog from the portal backend: Files of type lists the program's
    filters, a pattern typed in File name (*.md) filters the list, and the
    file double-clicked comes back as a file:// URI with the filter chosen."""
    empty = s.settle("desktop")
    start_portal(s)
    call = PortalCall(s, "OpenFile", "Open Test", in_documents(s, filters=text_filters()))
    s.wait_window(title="Open Test", state="activated")
    shot = s.settle("open")
    box = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None and box[2] > 500 and box[3] > 380, f"the Open dialog is up at {box}")
    x, y = box[0], box[1]
    # The List view: Letters, then Notes.txt (the type's filter keeps it).
    notes = (x + 108, y + 88, 140, 16)
    # File name has the focus: a pattern hides Notes.txt, *.txt shows it.
    s.inject("type", "*.md", "key", "Return", "sleep", 500)
    hidden = s.settle("pattern")
    s.check(hidden.differs(shot, notes, step=1) > 0.05, "*.md hid Notes.txt")
    s.inject("key", "ctrl+a", "type", "*.txt", "key", "Return", "sleep", 500)
    shown = s.settle("pattern-txt")
    s.check(shown.differs(shot, notes, step=1) < 0.01, "*.txt showed it again")
    s.inject("move", x + 150, y + 96, "click", "sleep", 100, "click")
    response, results = call.wait(s)
    s.check(response == 0, "Open answered OK")
    want = documents_uri(s, "Notes.txt")
    s.check(results.get("uris") == [want], f"it returned {want}")
    s.check(results.get("current_filter", (None,))[0] == "Text Documents (*.txt)",
            "with the filter chosen")  # fmt: skip
    s.check(results.get("writable") is True, "writable, as Open as read-only was off")
    s.wait_window(title="Open Test", gone=True)


@scenario(LUNA_INI)
def portal_save_as(s):
    """Save As adds the type's extension to a name without one, and asks
    before replacing a file that's there (Yes replaces it)."""
    from gi.repository import GLib

    start_portal(s)
    options = in_documents(s, filters=text_filters(), current_name=GLib.Variant("s", "Report"))
    call = PortalCall(s, "SaveFile", "Save Test", options)
    s.wait_window(title="Save Test", state="activated")
    s.settle("save-as")
    s.inject("key", "Return")
    response, results = call.wait(s)
    want = documents_uri(s, "Report.txt")
    s.check(response == 0 and results.get("uris") == [want], f"Save As returned {want}")
    s.wait_window(title="Save Test", gone=True)

    options["current_name"] = GLib.Variant("s", "Notes")
    call = PortalCall(s, "SaveFile", "Save Test", options, handle=PORTAL_HANDLE + "2")
    s.wait_window(title="Save Test", state="activated")
    asking = s.settle("save-as-again")
    s.inject("key", "Return")
    # The question is the dialog's own window (no taskbar button of its
    # own), so the dialog loses the focus to it.
    s.wait(lambda: "activated" not in s.window(title="Save Test")["states"],
           "Notes.txt already exists. Do you want to replace it?")  # fmt: skip
    replace = s.settle("replace")
    s.check(replace.differs(asking, (0, 0, WIDTH, HEIGHT - 40)) > 0.01, "the question shows")
    s.inject("key", "alt+y")
    response, results = call.wait(s)
    want = documents_uri(s, "Notes.txt")
    s.check(response == 0 and results.get("uris") == [want], "Yes replaced Notes.txt")


@scenario(LUNA_INI)
def portal_cancel(s):
    """Esc cancels the Open dialog (response 1), and the portal's
    Request.Close closes one (response 2)."""
    start_portal(s)
    call = PortalCall(s, "OpenFile", "Cancel Test", {})
    s.wait_window(title="Cancel Test", state="activated")
    s.inject("key", "Escape")
    response, _ = call.wait(s)
    s.check(response == 1, "Esc answered Cancel")
    s.wait_window(title="Cancel Test", gone=True)
    call = PortalCall(s, "OpenFile", "Close Test", {}, handle=PORTAL_HANDLE + "2")
    s.wait_window(title="Close Test", state="activated")
    call.close()
    response, _ = call.wait(s)
    s.check(response == 2, "Request.Close ended it")
    s.wait_window(title="Close Test", gone=True)


@scenario(AERO_INI)
def portal_folder(s):
    """A folder picker (directory), here in the Aero style (the same form,
    in its colours): only folders are listed, and the folder named comes
    back."""
    from gi.repository import GLib

    start_portal(s)
    call = PortalCall(s, "OpenFile", "Folder Test",
                      in_documents(s, directory=GLib.Variant("b", True)))  # fmt: skip
    s.wait_window(title="Folder Test", state="activated")
    s.settle("folder")
    s.inject("type", "Letters", "key", "Return")
    response, results = call.wait(s)
    want = documents_uri(s, "Letters")
    s.check(response == 0 and results.get("uris") == [want], f"it returned {want}")


def portal_frontend():
    """xdg-desktop-portal's own program, or None."""
    for path in ("/usr/lib/xdg-desktop-portal", "/usr/libexec/xdg-desktop-portal"):
        if os.access(path, os.X_OK):
            return path
    return None


GTK4_FILE_DIALOG = r"""
import sys
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, GLib

loop = GLib.MainLoop()
window = Gtk.Window(title="Owner Window")
window.set_default_size(400, 300)
window.present()

def done(dialog, result):
    try:
        print(dialog.open_finish(result).get_path(), flush=True)
    except GLib.Error as e:
        print("error: " + e.message, flush=True)
    loop.quit()

def ask():
    Gtk.FileDialog(title="GTK4 Test").open(window, None, done)
    return False

GLib.timeout_add(500, ask)
loop.run()
"""


def start_frontend(s):
    """This session's own xdg-desktop-portal, sending FileChooser to our
    backend as our portals.conf says; False if it isn't installed."""
    import gi

    gi.require_version("Gio", "2.0")
    from gi.repository import Gio, GLib

    frontend = portal_frontend()
    if frontend is None:
        return False
    start_portal(s)
    conf = os.path.join(os.environ["XDG_CONFIG_HOME"], "xdg-desktop-portal")
    os.makedirs(conf, exist_ok=True)
    shutil.copy(os.path.join(SOURCE_ROOT, "data", "xdg-desktop-portal",
                             "xeondesktop-portals.conf"), conf)  # fmt: skip
    portals = os.path.join(s.base, "portals")
    os.makedirs(portals, exist_ok=True)
    shutil.copy(os.path.join(SOURCE_ROOT, "data", "xdg-desktop-portal", "portals",
                             "xeondesktop.portal"), portals)  # fmt: skip
    s.launch(frontend, "--replace",
             env={"XDG_DESKTOP_PORTAL_DIR": portals, "XDG_CURRENT_DESKTOP": "XeonDesktop"})
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def frontend_up():
        try:
            pid = bus.call_sync(
                "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                "GetConnectionUnixProcessID",
                GLib.Variant("(s)", ("org.freedesktop.portal.Desktop",)),
                GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, 5000, None,
            ).unpack()[0]  # fmt: skip
            with open(f"/proc/{pid}/environ", "rb") as f:
                return b"XDG_DESKTOP_PORTAL_DIR=" + portals.encode() in f.read().split(b"\0")
        except (GLib.Error, OSError):
            return False

    s.wait(frontend_up, "this session's xdg-desktop-portal")
    return True


@scenario(LUNA_INI, requires=("gi",))
def portal_frontend_gtk4(s):
    """A GTK 4 program's GtkFileDialog (GDK_DEBUG=portals, as the session
    sets it) gets our dialog through xdg-desktop-portal, opened over its
    window through xdg-foreign: an owned window, without a taskbar button
    of its own. The file chosen is what the program gets."""
    import gi

    try:
        gi.require_version("Gtk", "4.0")
    except ValueError:
        s.log("skipped: GTK 4's introspection data isn't installed")
        return
    if not start_frontend(s):
        s.log("skipped: xdg-desktop-portal isn't installed")
        return
    empty = s.settle("desktop")
    env = dict(os.environ, GDK_DEBUG="portals", XDG_CURRENT_DESKTOP="XeonDesktop",
               GDK_BACKEND="wayland")  # fmt: skip
    program = subprocess.Popen([sys.executable, "-c", GTK4_FILE_DIALOG], env=env,
                               stdout=subprocess.PIPE, preexec_fn=die_with_parent)  # fmt: skip
    s.children.append(program)
    s.wait_window(title="Owner Window")
    # The dialog takes the focus from its owner, but has no window of its
    # own in the taskbar's list.
    s.wait(lambda: "activated" not in s.window(title="Owner Window")["states"],
           "the dialog over the owner window")  # fmt: skip
    shot = s.settle("gtk4")
    s.check(s.window(title="GTK4 Test") is None, "the dialog is the program window's own")
    box = shot.changed_box(empty, (0, 0, WIDTH, HEIGHT - 40))
    s.check(box is not None and box[2] > 500, f"the dialog shows at {box}")
    path = os.path.join(s.home, "Documents", "Notes.txt")
    s.inject("type", path, "key", "Return")
    out, _ = program.communicate(timeout=30)
    s.check(out.decode().strip() == path, f"the GTK 4 program got {path}")


# ---- showcase: the README's screenshots ----------------------------------------
#
# tools/screenshots.sh runs these (the Aero ones on the GPU, for blurred
# glass) and copies each one's last screenshot, labelled "showcase", into
# docs/screenshots. They run only when named.

WALLPAPERS = os.path.join(SOURCE_ROOT, "data", "wallpapers")


def showcase_ini(style, start_menu, scheme=None, wallpaper=None, extra=""):
    """A desktop.ini for a showcase: the style and Start menu, a colour
    scheme and one of our own wallpapers."""
    ini = f"[Appearance]\nStyle={style}\n"
    if scheme:
        ini += f"Scheme={scheme}\n"
    ini += f"\n[StartMenu]\nStyle={start_menu}\n"
    if wallpaper:
        ini += (f"\n[Desktop]\nWallpaper={os.path.join(WALLPAPERS, wallpaper)}\n"
                "WallpaperMode=fill\n")  # fmt: skip
    return ini + extra


def showcase_shot(s):
    """The README's screenshot: once everything has settled."""
    s.settle("settled")
    s.shot("showcase")


@scenario(showcase_ini("classic", "classic"), showcase=True)
def showcase_classic(s):
    """Classic: Windows 2000's Start menu, Programs and Accessories open,
    over My Computer."""
    s.launch("xd-files", "computer:///")
    s.wait_window(title="My Computer", state="activated")
    s.settle("my-computer")
    # P opens Programs; Right opens its first folder, Accessories.
    s.inject("key", "ctrl+Escape", "sleep", 600, "key", "p", "sleep", 600, "key", "Right",
             "sleep", 600)  # fmt: skip
    showcase_shot(s)


@scenario(showcase_ini("luna", "xp", "luna-blue", "Storm Hill.jpg"), setup=mock_laptop,
          requires=("dbusmock",), showcase=True)  # fmt: skip
def showcase_luna(s):
    """Luna (Blue): XP's Start menu over My Documents, a battery in the
    notification area."""
    s.launch("xd-files", os.path.join(s.home, "Documents"))
    s.wait_window(title="Documents", state="activated")
    s.settle("documents")
    s.inject("key", "ctrl+Escape", "sleep", 800)
    showcase_shot(s)


@scenario(showcase_ini("luna", "xp", "luna-olive", "Olive Forest.jpg"), showcase=True)
def showcase_olive(s):
    """Luna (Olive Green): Control Panel's categories."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    showcase_shot(s)


@scenario(showcase_ini("luna", "xp", "luna-silver", "Silver Factory.jpg"), showcase=True)
def showcase_silver(s):
    """Luna (Silver): My Computer with its task pane, and Folder Options.
    (Not System Properties: it shows the computer's own hardware.)"""
    s.launch("xd-files", "computer:///")
    s.wait_window(title="My Computer", state="activated")
    s.settle("my-computer")
    s.launch("xd-folders")
    s.wait_window(title="Folder Options", state="activated")
    showcase_shot(s)


@scenario(showcase_ini("luna", "xp", "luna-dark", "Energy.jpg"), showcase=True)
def showcase_dark(s):
    """Luna (Dark): Display Properties' Appearance tab."""
    s.launch("xd-desk", "--page", "3")
    s.wait_window(title="Display Properties", state="activated")
    showcase_shot(s)


@scenario(showcase_ini("aero", "win7", None, "Storm Hill.jpg"), setup=glass_setup,
          showcase=True)  # fmt: skip
def showcase_aero(s):
    """Aero: Windows 7's Start menu over a window, on blurred glass."""
    s.launch("xd-files", os.path.join(s.home, "Pictures"))
    s.wait_window(title="Pictures", state="activated")
    s.settle("pictures")
    s.inject("key", "ctrl+Escape", "sleep", 800)
    showcase_shot(s)


@scenario(showcase_ini("aero", "win7", None, "Storm Hill.jpg"), setup=glass_setup,
          showcase=True)  # fmt: skip
def showcase_aero_snap(s):
    """Aero Snap: Control Panel and Task Manager side by side."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.inject("key", "super+Left")
    s.launch("xd-taskmgr")
    s.wait_window(title="Task Manager", state="activated")
    s.inject("key", "super+Right", "sleep", 300, "key", "ctrl+Tab", "key", "ctrl+Tab")
    s.inject("move", WIDTH // 2, HEIGHT // 2, "sleep", 2500)
    showcase_shot(s)


@scenario(showcase_ini("aero", "win7", None, "Storm Hill.jpg"), setup=glass_setup,
          showcase=True)  # fmt: skip
def showcase_aero_thumbnails(s):
    """Aero: a program's window thumbnails above its taskbar button."""
    bar = (0, HEIGHT - 40, WIDTH, 40)
    before = s.settle("taskbar")
    for folder in ("Documents", "Pictures"):
        s.launch("xd-files", os.path.join(s.home, folder))
        s.wait_window(title=folder, state="activated")
    after = s.settle("two-windows")
    button = after.changed_box(before, bar)
    s.inject("move", button[0] + button[2] // 2, HEIGHT - 20, "sleep", 1200)
    showcase_shot(s)


@scenario(showcase_ini("aero", "win7", None, "Storm Hill.jpg"), setup=glass_setup,
          showcase=True)  # fmt: skip
def showcase_alt_tab(s):
    """Alt+Tab's window over three windows, on glass."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.launch("xd-files", os.path.join(s.home, "Documents"))
    s.wait_window(title="Documents", state="activated")
    s.launch("xd-desk")
    s.wait_window(title="Display Properties", state="activated")
    s.settle("three")
    held = subprocess.Popen([s.tool("xd-inject"), "hold", "alt", "key", "Tab", "sleep", "5000",
                             "release", "alt"])  # fmt: skip
    time.sleep(1.5)
    s.shot("showcase")
    held.wait(timeout=30)


@scenario(showcase_ini("whistler", "xp", None, "Watercolor Blue.jpg"), showcase=True)
def showcase_watercolor(s):
    """Watercolor: Taskbar and Start Menu Properties over My Computer."""
    s.launch("xd-files", "computer:///")
    s.wait_window(title="My Computer", state="activated")
    s.settle("my-computer")
    s.launch("xd-taskbar")
    s.wait_window(title="Taskbar and Start Menu Properties", state="activated")
    showcase_shot(s)


@scenario(showcase_ini("luna", "xp", "luna-blue", "Storm Hill.jpg"), requires=("dbusmock",),
          showcase=True)  # fmt: skip
def showcase_date_time(s):
    """Date and Time Properties' Time Zone tab with its map."""
    mock_timedate(s, {"SystemNTPServers": ["time.example.org"],
                      "ServerName": "time.example.org", "Received": time.time() - 3600})
    s.launch("xd-timedate", "--page", "1")
    s.wait_window(title="Date and Time Properties", state="activated")
    showcase_shot(s)


@scenario(showcase_ini("luna", "xp", "luna-blue", "Storm Hill.jpg"), showcase=True)
def showcase_minefield(s):
    """Minefield, a game under way."""
    desktop = s.settle("desktop")
    s.launch(s.tool("xd-mines"), "--seed", "5")  # its first click opens a good area
    s.wait_window(title="Minefield", state="activated")
    board = s.settle("board")
    x, y, w, h = board.changed_box(desktop, (0, 0, WIDTH, HEIGHT - 40))
    # The 9 x 9 squares fill the window's lower part.
    s.inject("move", x + w // 2, y + h - h // 3, "click", "sleep", 300, "move", x - 40, y + h // 2)
    showcase_shot(s)


@scenario(showcase_ini("luna", "xp", "luna-blue", "Storm Hill.jpg"), showcase=True)
def showcase_shut_down(s):
    """XP's Turn Off Computer, from the Start menu."""
    s.inject("key", "ctrl+Escape", "sleep", 600, "key", "u")
    s.wait_window(title="Shut Down XeonDesktop", state="activated")
    showcase_shot(s)


@scenario(showcase_ini("classic", "classic"), showcase=True)
def showcase_security(s):
    """Ctrl+Alt+Delete: XeonDesktop Security."""
    s.launch("xd-control")
    s.wait_window(title="Control Panel", state="activated")
    s.settle("control-panel")
    s.inject("key", "ctrl+alt+Delete", "sleep", 1500)
    showcase_shot(s)


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
        print("\n".join(suite()))
        return 0
    if args.list:
        for name, (fn, _, _) in SCENARIOS.items():
            mark = " (showcase)" if SCENARIO_OPTIONS[name]["showcase"] else ""
            print(f"{name}{mark}: {' '.join(fn.__doc__.split())}")
        return 0
    if args.inner:
        return inner(args)
    return outer(args)


if __name__ == "__main__":
    sys.exit(main())
