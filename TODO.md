# TODO

Everything unfinished, untested or known to be wrong. Keep this current:
anything that ships incomplete gets a line here. "Untested" means the code
exists but nobody has exercised it with real input or on real hardware.

Legend: **[ ]** not done, **[~]** partly done, **[?]** done but untested.

## Platform and session

- [~] **Real hardware**: runs as a full session from SDDM on DRM/KMS with
      logind, in a VMware VM (vmwgfx without 3D, so pixman software
      rendering: wlroots won't pick it on DRM by itself, so the compositor
      falls back to it when GLES2 and Vulkan fail). Untested: a real GPU
      (GLES2/Vulkan on DRM), physical monitors and their modes, hotplug,
      libinput devices beyond the VM's, VT switching, suspend/resume.
- [~] **Multiple monitors**: tested with two headless outputs: a taskbar
      and wallpaper on each, dragging a window across and maximizing it on
      the second, turning one off (its windows move over), rearranging them
      in Display Properties; scales 1 and 1.5 side by side, the Start menu,
      window menu and maximize on the second, desktop icons on the primary
      (UI test two_monitors). Untested: real monitors, hotplug.
- [?] **Automatic per-output scale** from EDID size: untested (only the
      `--scale` override has been used).
- [~] Scaling smoke test (2026-10-15, nested 1024x768 with --scale 1.25,
      1.5 and 2): the taskbar, all three Start menus and the 2000 Programs
      cascade, window frames and caption buttons (minimize, restore from
      the task button, maximize at 1.5), Display Properties, Run, xd-files,
      Properties and the Task Manager all render crisply and line up.
      Found and fixed: the Classic Start button missed clicks at the very
      bottom edge (now Start and task buttons reach the screen edges).
      Open: the XP Start menu and Display Properties are taller than a
      512px logical screen (1.5x on 768px) and get cut off; XP shrinks the
      most-used list and Windows keeps dialogs' title bars on screen. GTK 3
      draws at the next whole scale and is downscaled at fractional scales
      (no fractional-scale support in GTK 3). Not covered: scales on
      several outputs at once, changing scale while running, cursor size.
- [~] **Session entry**: `wayland-sessions/xeondesktop.desktop` runs
      `xeondesktop-session` (sets XDG_CURRENT_DESKTOP and friends, loads
      `~/.config/environment.d` through systemd's generator, starts the
      compositor); outside a nested session the compositor passes
      WAYLAND_DISPLAY, DISPLAY and GTK_THEME on to D-Bus/systemd activated
      services; `xeondesktop-portals.conf` picks the GTK and wlr portals.
      Tested: the environment.d import, `meson install` through the Debian
      and Arch packages (built, installed and started in containers).
      **Untested: logging in from a display manager** (GDM, SDDM, greetd),
      portals in such a session.
- [~] **XWayland**: X11 apps run (Xwayland starts when the first one
      connects; DISPLAY is set for everything launched from the session).
      Tested with xterm: our frame, title and taskbar button, typing,
      maximize/restore, move, cascade placement, fullscreen, Alt+F4, its
      Ctrl+click menu (drawn over the taskbar), clean exit. Untested: X11
      dialogs/transients, Motif-undecorated windows, size-hint-fixed
      windows, apps that place themselves, HiDPI (Xwayland draws at 1x and
      is upscaled), clipboard and drag-and-drop between X11 and Wayland.
- [~] **Drag and drop** between Wayland programs (the compositor had none),
      with the drag icon above everything, and the primary selection
      (middle-click paste, which had no handler). Tested with GTK test
      programs. While a button is held after a press in a client, the
      pointer stays with that surface (the implicit grab; before, focus
      followed the pointer). Untested: X11 <-> Wayland drags, real file
      managers.
- [~] **Keyboard**: `[Keyboard]` in desktop.ini (Layout, Variant, Options,
      Model, RepeatDelay, RepeatRate) applies to every keyboard, live; empty
      names fall back to `XKB_DEFAULT_*`, a bad layout to the default with a
      log message. Tested in a nested session: the bad-layout fallback and a
      live change to `de` (compiled and applied; typing in it not checked).
      The Keyboard applet and XP's minimized Language bar ("EN", left of
      the tray, with more than one layout: a menu switches the session's
      layout; the compositor reports switches by keys too).
- [~] **Output management** (`wlr-output-management`) and output power:
      tested with wlr-randr (scale, off/on, two outputs). Windows on an
      output that's turned off move to the nearest one, maximized ones
      refit. Settings are remembered per monitor (make, model and serial,
      or the connector) in `monitors.ini` and restored when it appears; the
      remembered arrangement is shifted to start at 0,0 when monitors are
      missing. Untested: real monitors, mode/refresh changes (headless and
      nested outputs have one mode), rotation, hotplug, a missing monitor
      in the middle of an arrangement (leaves a gap).
- [~] **Idle and screen lock** (`ext-idle-notify`, idle-inhibit,
      `ext-session-lock`): tested with swayidle (idle after the timeout,
      resume on input, held off while mpv plays) and swaylock (locks,
      blocks shortcuts but Ctrl+Alt+Backspace, stays locked if the locker
      dies, a new locker can take over, unlocks, keyboard returns).
- [~] **xdg-activation** with Windows' foreground rules: a token may take
      the focus if the focused app, the shell or nobody (no window focused)
      asked for it; otherwise the window's taskbar button flashes (seven
      times, then stays lit until it's activated), as with FlashWindow. The
      shell gives every program it starts a token, so a second instance can
      raise the first. X11 windows: `_NET_ACTIVE_WINDOW` focuses only within
      the focused app, and the urgency hint flashes. Tested: a GTK window
      calling present() while another app has focus (flashes, in all three
      styles), Task Manager started twice from the taskbar menu (raised).
      Untested: X11 activation and urgency. wlroots drops tokens whose
      input serial the app never got, so those requests do nothing at all.
- [~] **Lock Computer** (`xd-lock`, Win+L, the Windows 7 menu's Lock):
      Windows 2000's "Computer Locked" screen; Ctrl+Alt+Del opens "Unlock
      Computer", checked with PAM on a worker thread. If the locker dies
      while locked, the compositor starts it again (5 times a minute at
      most). Tested: locking, Ctrl+Alt+Del, a wrong password and its
      message, Caps Lock warning, restart after a crash, a keyboard
      unplugged and replugged, and (in a real session, with the installed
      `xeondesktop` PAM service) locking with Win+L and unlocking with the
      right password. The PAM service file
      (`/etc/pam.d/xeondesktop`, `auth include login`) is installed by
      `meson install`; until then `login` is used. Every monitor is
      covered, the dialog on the one with the pointer; logind's Unlock
      (Switch User) unlocks. The locker hears it's locked once every
      monitor has shown the cover (a second at most for one that shows
      nothing), and the shell then: the computer sleeps, and Switch User
      goes to the greeter, only once the screens are locked (Switch User
      not at all if they aren't within 5 seconds). While locked with the
      locker gone, only the one the compositor starts again may take
      over. (Change
      Password is in Windows Security, below.)
- [~] **Screen savers**: Blank Screen, Starfield Simulation, Mystify Your
      Mind, Beziers, Marquee Display and Flying Penguins, started by the
      shell after [ScreenSaver] Wait minutes idle (held off by video
      players), password protected or not; monitors off after MonitorOff
      minutes (wlr-output-power). Tested in a headless session (with
      XD_IDLE_SECONDS=1). Each saver's Settings
      ([ScreenSaver.*]), 3D Pipes and 3D Text drawn in software. Not yet:
      hardware tests of monitor power; textured surfaces.
- [x] **Priority**: a display manager can run at background priority
      (CachyOS's ananicy-cpp types sddm as a "Launcher": nice 16, idle I/O)
      and the session inherited it, delaying input by seconds. The
      compositor raises itself (nice -10 as far as RLIMIT_NICE allows,
      normal I/O, SCHED_RESET_ON_FORK for its children) and ships an
      ananicy-cpp rule like KWin's and Sway's. Tested on CachyOS.
- [ ] Power actions (Shut down / Restart / Stand by / Hibernate) have only
      ever run **simulated** in nested sessions; never executed for real.

## Packaging

- [~] GitHub Actions builds a `.deb` (Debian testing and unstable, the only
      releases with wlroots 0.20) and an Arch Linux / CachyOS package on
      every push and pull request, and attaches them to a release for v*
      tags. The Debian dependencies come from dpkg-shlibdeps; the Arch ones
      were checked with namcap. Built, tested and installed locally in
      podman containers (debian:testing, archlinux, cachyos/cachyos,
      ubuntu:26.04). Ubuntu 26.04 LTS has wlroots 0.19, so its package
      builds our pinned wlroots 0.20 (subprojects/wlroots.wrap) in
      statically. The workflow and the release job pass on GitHub (v0.3.0).
      Man pages for every program (lintian is clean but for two
      desktop-entry-lacks-keywords notes, Files and Task Manager). The
      .debs are also built for arm64 (ubuntu-24.04-arm runners); the Arch
      and CachyOS images are x86-64 only. Not yet: arm64 seen building on
      GitHub, an apt/pacman repository to install from.

## Compositor

- [~] Window menu from the window itself: Alt+Space, a click on the caption
      icon, a right-click on the title bar and xdg_toplevel.show_window_menu
      all open the shell's window menu (via xd-window-arranger-v1). Tested:
      Alt+Space, icon click, right-click, double-clicking the icon to close.
      A client's show_window_menu (a GTK 3 header bar's right-click) opens
      it at the pointer; the menu's access keys (R, M, S, N, X, C) work,
      their underlines shown at once when Alt+Space opened it. UI tests:
      client_window_menu, window_menu_letters.
- [~] Windows that ask to draw their own decorations (xdg-decoration
      client-side, or the KDE protocol's client mode, e.g. Firefox with tabs
      in the title bar) get no XP frame, plus an invisible 6px resize margin.
      Tested with Firefox: its own close button and edge resizing work.
      Firefox users can tick Customize Toolbar > Title Bar to get our frame.
- [x] Clients that never negotiate but draw their own title bar (GTK 4 /
      libadwaita) are detected from their shadow margins and get no frame
      (tested with pavucontrol). `[Windows] AlwaysDrawFrames=true` forces
      our frame on every window.
- [x] Client-decorated windows cascade like the others. Their window menu
      and keyboard move/size: untested.
- [x] Fullscreen (tested with Firefox F11 and `xterm -fullscreen`): covers
      the taskbar while in front; Alt+Tab away drops it under the window
      switched to, with the taskbar back.
- [x] Owned dialogs open centred over their owner, stay above it, minimize
      and restore with it, and have no taskbar button.
- [x] Win key alone and Ctrl+Esc open the Start menu.
- [~] Ctrl+Alt+Delete: in the Classic style Windows 2000's Windows
      Security dialog (`xd-security`: logon name and date, Lock Computer,
      Log Off, Shut Down, Change Password, Task Manager); in the XP styles
      Task Manager, as XP does with the Welcome screen. Ctrl+Shift+Esc is
      always Task Manager. Change Password drives `passwd` on a pty in the
      C locale. Tested: the dialogs, mismatched new passwords, a wrong old
      password through the real passwd, and success / wrong old password /
      a too-short new one against a fake passwd (unit test). **Untested:
      really changing a password** (please try it), its buttons other than
      Change Password, non-English passwd prompts (passwd runs in the C
      locale, so these shouldn't occur).
- [~] Aero glass: xd-compositor-fx is the compositor built on SceneFX
      (subprojects/scenefx.wrap, pinned; packages fetch it with
      packaging/fetch-subprojects.sh), whose scene graph blurs what's behind
      a node. In the Aero style each window gets a blur node masked by its
      frame's alpha (the frame leaves the client area transparent) and a
      soft shadow (none when maximized), and the shell's taskbar a blur
      masked by its pixels. SceneFX renders with GLES2 only: xd-compositor
      hands over to xd-compositor-fx when it's installed beside it (not
      when WLR_RENDERER asks for software, nor with XD_NO_FX set), and
      xd-compositor-fx hands back when there's no GPU renderer; it sets
      XD_GLASS=1 for the session, without which Aero draws opaque, as
      Windows 7 Basic. Virtual machines' GPUs (vmwgfx, virtio_gpu, qxl)
      draw in software and stay opaque: VirtualBox's 3D measured 20-50 ms
      a frame (with or without blur) against 1 ms in software, enough to
      freeze the session with Firefox open. XD_VM_GPU=1 uses it anyway;
      there the cursor is drawn in software (vmwgfx's hardware cursor
      shows nothing), and a SceneFX patch allocates its effect buffers in
      BGRA. Tested: the hand-over and hand-back, Aero Basic, the VM check,
      the blur buffers on VirtualBox 3D (headless). The Windows 7 Start
      menu and taskbar are blurred too (checked headless on vmwgfx's GLES).
      Window Color and Appearance: 16 swatches, transparency, intensity,
      mixer. **Untested: how the blur looks on a real GPU.**
- [x] XP's "Animate windows when minimizing and maximizing" ([Effects]
      AnimateWindows, on by default): a bar in the active caption colour
      flies from the title bar to the taskbar button and back (the shell
      reports buttons with set_rectangle), and between the two title bars
      on maximize/restore down; a restored window shows when it arrives.
      Tested headless (minimize, captured mid-flight).
- [x] Windows 7's window handling in the Aero style (snap.c): Aero Snap
      (the top edge maximizes, the left/right edges fill that half, with a
      glass preview under the window; a snapped window gets its size back
      when dragged away; Win+Left/Right, Win+Down restores), maximized
      windows restore when dragged by the caption, Aero Shake (minimizes
      the other windows; shaking again brings them back). Tested headless:
      snap left, preview right, drag back out, shake and shake again.
- [x] Aero Peek: the taskbar's Show desktop strip at the far end (Aero):
      hovered half a second, every window fades to its outline
      (xd-window-arranger-v1 version 6, peek); a click shows the desktop;
      Win+Space peeks while the Windows key is held. Tested headless.
- [x] Windows 7's keys across monitors (Aero): Win+Left/Right on a
      window already snapped that way go on to the next monitor's far half
      (round the end; with one monitor, left half, right half, restored);
      Win+Shift+Left/Right move a window to the next monitor (maximized and
      snapped windows stay so there, and restore there); Win+Shift+Up makes
      it as tall as the work area, Win+Down puts it back. Tested headless
      with two outputs.
- [~] Windows 7's taskbar, the unified one (Aero's by default; any
      style's with Taskbar Properties' "Use the unified taskbar", [Taskbar]
      Layout): pinned and running programs share a row
      of icon buttons (taskbar-pinned; the browser, Files and a media
      player by default), running ones framed with stacked edges for more
      windows; click activates or cycles, middle-click starts another,
      dragging reorders pins. Jump lists: Recent (recently-used.xbel, per
      program), Tasks (desktop actions), pin/unpin, close. Taskbar
      Properties' "Taskbar buttons" choice (icon buttons, or labelled
      ones as XP's) and "Use small icons" ([Taskbar] Icons: 32px icons on
      a 40px taskbar, or 16px ones on XP's 30px; XP's task list takes
      24px icons when large). In Classic, Luna and Watercolor the buttons
      are the style's own task buttons, a pinned program's a flat toolbar
      button as Quick Launch's; their jump lists are still Windows 7's
      look. Thumbnails show on XP's task list too ([Taskbar] Previews,
      "Show window previews"), drawn as the style's tooltips outside Aero.
      Tested headless in all four styles. Live thumbnails with Aero Peek
      (ext-image-copy-capture, only for our shell), Windows 7's jump list
      look with pinned items, pins by dropping files or launchers,
      combining as needed (least recently used first). Tested headless.
      Jump list items' right-click menus (Open, Pin/Unpin, Remove from
      this list, Properties), dragging pinned items to reorder them,
      thumbnails stacked beside side taskbars, and Previous/Play-Pause/
      Next under a media player's thumbnail through MPRIS (only for one
      window with one matching player). Windows' own thumbnail toolbars
      have no Linux counterpart. Sizes and the items' menus are from
      memory; no keyboard way to open an item's menu; removing the last
      item of the Start menu's jump list leaves it empty.
- [x] A window resized from its left or top edge snapped back to its old
      place at its next commit after being moved (the resize's anchor was
      never released), and windows that redraw constantly (Firefox) fought
      the drag. UI test: move_after_left_resize.
- [x] Resizing from the top/left edges keeps the opposite edge still.
- [x] Tested: Alt+Tab, Alt+F4 (Wayland and X11), Win+Up/Down (maximize,
      restore, minimize), Ctrl+Alt+T, Ctrl+Alt+Backspace, Ctrl+Esc,
      dragging borders and corners, the caption's minimize, maximize and
      close buttons, double-clicking the caption icon to close, and
      SIGTERM (Log Off) ending the session cleanly.
- Debug builds print LeakSanitizer reports from fontconfig/GTK caches at
  exit; only the unit tests use the suppression file.

### Protocols other compositors have

- [x] Pointer lock and confinement (pointer-constraints, relative-pointer):
      games, Blender, VMs and remote desktop can't capture the mouse.
- [~] Touchscreens and drawing tablets (touch, tablet-v2): ignored on
      purpose in input.c today. Done, untested on real hardware: touch to
      programs that read it, else as the mouse; pens with pressure and
      tilt, else as the mouse; pads. Still to do: press-and-hold right
      click, touch feedback, a setting for which monitor a touchscreen or
      tablet maps to (Windows 7's Tablet PC Settings).
- [x] Touchpads: Mouse Properties' Touchpad tab ([Touchpad]; shown when
      udev knows a touchpad): tap to click (two fingers tapped right-click,
      three middle-click), tap and drag with optional drag lock, two-finger
      or edge scrolling, reverse (natural) scrolling, scroll speed, the
      right-click by the bottom right corner or two fingers, off while
      typing or while a mouse is plugged in, and the touchpad's own speed
      and acceleration, each only where libinput says the touchpad has it.
      Gestures (pointer-gestures): pinches, holds and two-finger swipes go
      to the program under the pointer; three-finger swipes are the
      desktop's, as on Windows' precision touchpads (down shows the
      desktop, up brings the windows back, sideways switches windows
      through Alt+Tab's window), unless turned off there. Tested headless
      (UI tests touchpad_pinch, touchpad_desktop_swipes,
      touchpad_swipes_to_programs, touchpad_settings_tab); libinput's own
      options untested on a real touchpad. Not done: four-finger gestures,
      pinch to zoom the desktop.
- [x] Letting a program take all shortcuts (keyboard-shortcuts-inhibit,
      and the X11 programs' xwayland-keyboard-grab): VMs and remote
      desktop can't have Alt+Tab or the Windows key.
      Done: kept are Ctrl+Alt+Delete, Win+L, Ctrl+Alt+F1..F12 (which now
      switch virtual terminals), Ctrl+Alt+Backspace, power and
      accessibility keys; Ctrl+Alt alone releases. Untested with real
      VirtualBox, VMware, games and Remmina.
- [~] Explicit GPU sync (linux-drm-syncobj): NVIDIA's own driver flickers
      and stutters without it; before bare metal GPU testing.
- [~] Games: variable refresh rate (adaptive sync, with a Display setting),
      tearing-control for lower latency, and the frame pacing protocols
      recent Mesa uses (fifo, commit-timing). VRR (Display Properties >
      Settings > Advanced > Monitor; full-screen windows only, or
      AdaptiveSync=always in monitors.ini) and tearing (the async hint,
      while scanned out directly) are done, untested on real hardware;
      fifo and commit-timing wait for wlroots, which 0.20 lacks.
- [x] Sandboxes (security-context-v1): sandboxed programs (Flatpak's) get
      none of the protocols reaching past their own windows: screen
      capture, data-control, foreign-toplevel management, output
      management and power, gamma, layer-shell, virtual keyboard, session
      lock, our capture and arranger (trust.c). Programs on the desktop
      keep them, as other wlroots desktops give them (screenshot tools,
      clipboard managers). The shell, and a locker started again while
      locked, run on connections the compositor made for them
      (WAYLAND_SOCKET), known by those rather than their PIDs; Task
      Manager and input methods by the installed program they ran when
      connecting, refused if made to load other code. UI test
      sandbox_globals (tools/xd-globals).
- [ ] Window parenting (xdg-foreign, xdg-dialog): portal dialogs aren't
      tied to the window of the Flatpak program that asked, and dialogs
      can't be marked modal.
- [ ] Icons programs send for their windows (xdg-toplevel-icon), for those
      without a .desktop file.
- [ ] Minor: DRM leasing (VR headsets), HDR colour management
      (color-management-v1), content-type hints, alpha-modifier.

## Shell (taskbar, Start menus, desktop)

- [x] Standalone taskbar for other wlroots compositors: `-Dcompositor=false`
      builds without wlroots 0.20 (CI builds it too); `xd-shell
      --taskbar-only` (no desktop layer); `xd-shell --toggle-start-menu`
      (D-Bus org.xeondesktop.Shell) for a key binding;
      xeondesktop-taskbar.service (installed, not enabled); Log Off through
      logind's TerminateSession when our compositor isn't the session's.
      Tested only under our own headless compositor (labwc, sway not
      installed). KDE Plasma would show no task buttons (no
      wlr-foreign-toplevel); GNOME has no layer-shell; another panel's
      StatusNotifierWatcher (waybar) competes for the tray.
- [~] Notification area: StatusNotifierItem icons (we're the watcher, or a
      host of another desktop's), named or pixmap icons, attention icons,
      Passive items hidden, tooltips, Activate / SecondaryActivate, and
      dbusmenu menus (separators, checks, submenus). The clock is sized to
      each style's margins and shows the date as a tooltip. Tested with a
      test item on a private bus. Hide inactive icons (on by default):
      programs' icons quiet for three minutes fold behind a chevron that
      shows them for 15s (tested with XD_TRAY_INACTIVE_SECONDS=3). The
      clock: double-click or right-click > Adjust Date/Time opens Date and
      Time Properties (below); it used to show each new minute a minute
      late. Untested: real applets (Discord, Steam, Telegram), another
      desktop's watcher, radio items. Customize Notifications (Taskbar
      Properties > Customize...): every icon seen, each Hide when inactive,
      Always hide or Always show ([Notifications] by the item's Id).
- [~] Date and Time Properties (xd-timedate, through timedated and polkit):
      Date & Time (calendar, an analog clock, the time), Time Zone and
      Internet Time (timesyncd on or off). Tested against dbusmock's
      timedated: SetTimezone, SetNTP and SetTime with the right values.
      **Untested against the real timedated** (it would change this
      machine's clock). The Time Zone tab draws XP's world map from Natural
      Earth's land outlines, with the zone's band lit and its city marked.
      Internet Time is XP's: the servers timesyncd uses (timesync1),
      Update Now (restarts timesyncd), a typed server through
      SetRuntimeNTPServers (kept until restart; keeping it for good needs
      a root helper writing timesyncd.conf.d); greyed with a reason under
      chrony or ntpd. The hardware clock in local time (SetLocalRTC), for
      dual boot. Tested against dbusmock (UI tests timedate_*). Not yet:
      12-hour time entry, styled spin buttons.
- [~] Our own applets, drawn by the theme engine: volume (libpulse; XP's
      flyout with slider and Mute, the wheel, Mute and pavucontrol in the
      menu), network (NetworkManager: wired / Wi-Fi with signal, networks in
      range, saved ones activated, new ones added with the key from
      xd-wifi-key, Disconnect, Wi-Fi on/off, nm-connection-editor), battery
      (UPower, XP's remaining-time tooltips) and Bluetooth (BlueZ: on/off,
      connect/disconnect paired devices, Blueman). Tested against
      python-dbusmock's NetworkManager, UPower and BlueZ on a private bus
      and a private PipeWire with a null sink. **Untested on real
      hardware**: actual Wi-Fi connects (WPA3/SAE, WEP, hidden networks),
      a real battery, real Bluetooth devices. A double click on the
      network icon opens XP's connection Status (General and Support
      tabs); the volume flyout takes the arrow keys, Page Up/Down,
      Home/End, Space and Esc (step sizes are guesses); XP's low and
      critical battery alarms (UI test battery_alarms); Open Power Meter.
      Not yet: pairing new Bluetooth devices, enterprise (802.1X) networks
      in our own UI (nm-connection-editor opens instead), access keys in
      the tray icons' own menus.
- [~] Task buttons: the application's icon (desktop file found from the
      app id or WM_CLASS); tooltips with the whole title when it's cut
      short; pages with up/down arrows (and the wheel) when buttons would
      get narrower than 56px; Group similar taskbar buttons ("3 Firefox",
      a menu of its windows, Minimize Group / Close Group) once they'd get
      narrower than 100px; the group's right-click has XP's Cascade, Tile
      Horizontally/Vertically (xd-window-arranger-v1 v3's arrange_app),
      Minimize Group and Close Group. A drag held over a task button
      brings its window forward. Tested in Luna with 30 and 14+2 windows.
      The window frame's caption shows the application's icon too.
- [~] Taskbar options in Taskbar and Start Menu Properties: Auto-hide
      (tucked to 2px, out on hover or a menu), Keep on top (off: the bottom
      layer), Show the clock, Show the taskbar on all displays (off: only
      the first monitor's); Lock the Taskbar hides the toolbar grips.
      Tested headless, two outputs for all displays. Untested: Auto-hide
      with the Windows key, real monitors. With all displays off, Alt+Space
      opens the window menu on monitors without a taskbar too.
- [~] Taskbar on any edge and in several rows ([Taskbar] Edge, Rows,
      SideWidth): rows of task buttons and tray icons on top and bottom
      taskbars, a stacked layout on side ones; menus, paging, grouping,
      Auto-hide and drops follow the edge. Unlocked, it's dragged to
      another edge and resized by its inner edge, as in XP. Tested
      headless in Luna and Classic: all four edges, 1-3 rows, moving and
      resizing by mouse, the Start menus, the window menu from any edge, the
      clock's day and date on a tall taskbar, Quick Launch's Show Text.
      Untested: Watercolor, real monitors. Not yet: the date on side
      taskbars.
- [~] Right-click menus (XP): the taskbar's (Toolbars ▸ Quick Launch, Cascade,
      Tile Horizontally/Vertically and Undo, Show the Desktop, Task Manager,
      Lock the Taskbar, Properties) and a task button's window menu
      (Restore, Move, Size, Minimize, Maximize, Close; the default in bold),
      and the
      Start button's (Open, Explore, Properties, Open/Explore All Users).
      Move and Size run the compositor's keyboard move/size (arrows 8px, Ctrl
      1px, mouse too; Enter or a click keeps it, Esc undoes it). Tested:
      Move by arrows, Size by arrows, Esc restoring, the mouse during
      keyboard move/size, Ctrl's 1px steps, corners (UI test
      keyboard_move_size). The clock's menu
      adds Adjust Date/Time; Quick Launch's has its own (below). Not yet:
      the Address, Links, Desktop and New Toolbar toolbars, Search... in
      the Start button's menu. Start button menu: Open tested; Explore,
      Properties and
      the All Users items untested. Tested: the taskbar menu, Tile Vertically, Undo Tile
      showing up, Minimize and the minimized window's menu, Cascade, Tile
      Horizontally, Undo, Lock toggling (UI test taskbar_menu_arrange).
      Untested: Restore, Maximize, Close from the menu, several monitors.
      Every shell menu has Windows' access keys, underlined, hidden until
      the keyboard is used when [Effects] HideKeyboardCues is on.
- [~] Quick Launch: tooltips (the shortcut's name); a shortcut's menu
      (Open, Delete, Properties) and the toolbar's (Open Folder, Close
      Toolbar); files and applications dropped on it become shortcuts
      where they land (a mark shows where), and its buttons can be dragged
      along to rearrange them. xd-files drags its selection out (to Quick
      Launch or any program). Tested headless with a GTK drag source and
      xd-files. Dragging from our own Start menu, View > Large/Small Icons,
      Show Text, Show Title work too.
- [x] Desktop icons (deskicons.c), on the first monitor: My Documents, My
      Computer, Recycle Bin (empty/full icon), then the Desktop folder
      (launchers show their program's name and icon), watched live. XP's
      75x75 grid in columns, white labels with drop shadows, two lines
      unless selected; click, rubber-band selection, dragging icons to
      other cells (kept in the "desktop-icons" state file), double-click
      opens (folders and the special ones in xd-files, documents in their
      program, launchers run). Menus: Open, Delete, Rename, Properties;
      My Computer's Properties are System Properties, the Recycle Bin's
      menu empties it; the background's Arrange Icons By, Refresh, New ▸
      Folder / Text Document and Properties (Display Properties). Delete,
      Rename and Empty Recycle Bin run xd-files (--trash, --rename,
      --empty-trash) for its dialogs. Tested headless.
- [x] Desktop icons: a click gives the desktop the keyboard (an
      on-demand layer surface; the compositor deactivates the front
      window, as Windows does). Arrows, Home/End, Enter, Alt+Enter,
      Delete/Shift+Delete, F2, F5, Ctrl+A, Ctrl+Space, Shift+F10/Menu,
      type-ahead, Alt+F4 for Shut Down; the dotted focus once a key is
      used. Ctrl+click and Shift+click. Rename in place (F2, the menu, or
      a slow second click on the name), the extension left unselected.
      Cut/Copy/Paste: the shell owns the clipboard in the file managers'
      format (cut icons faded) and pastes through xd-files --copy-to /
      --move-to (its progress and replace dialogs). Tested headless:
      focus, Ctrl+click, arrows, rename, copy from xd-files and paste on
      the desktop, cut on the desktop and paste in xd-files, Alt+F4,
      type-ahead.
- [x] Desktop icons and drag and drop: dragging files' icons is a real
      drag (a faded drag image of the selection) that other programs can
      take; dropped back on the desktop they move on the grid, onto a
      folder icon they move into it (Ctrl copies), onto the Recycle Bin
      they're trashed. Files dragged in from other programs move or copy
      into the Desktop folder where they're dropped, or into the folder
      or Recycle Bin under the pointer (lit while hovered). The shell
      repeats held keys; a failed rename says why (xd-message). Tested
      headless: moving on the grid, onto a folder, desktop -> xd-files,
      xd-files -> desktop, onto the Recycle Bin (asks), key repeat.
- [x] Desktop icons: My Network Places (with gvfs's network backend),
      Display Properties' Customize Desktop (which icons show, Change
      Icon..., Restore Default), Auto Arrange and Align to Grid (free
      placement remembered), Show Desktop Icons, Windows 7's View/Sort by,
      icon sizes and "Use large icons". Tested headless. Not yet: XP's
      icon picker (Change Icon uses an image chooser), the Web tab, Desktop
      Cleanup; the Desktop Items dialog gets its own taskbar button
      (libxdui windows can't have an owner).
- [~] Windows XP Start menu. Done: layout in Luna, Watercolor and Classic,
      two-line Internet/E-mail entries, most used programs, cascading All
      Programs, Recent Documents and Control Panel submenus, Log Off and
      Turn Off Computer, Search (xd-search), Help and Support (xd-help),
      Set Program Access and Defaults (xd-defaults), Printers and Faxes,
      the user's full name in the header, keyboard navigation of every
      column and the footer. Pinning from All Programs, letter
      shortcuts, the highlight for newly installed programs, Customize
      Start Menu (large/small icons, number of programs, Clear List); all
      tested headless. Drawn underlines (All Programs, Run, Log Off, Turn
      Off Computer), the "New programs installed" balloon, Send To
      (Desktop, My Documents) and Properties in a program's menu. Not
      yet: Send To's other targets; the right column's own access keys
      (unknown).
- [x] Windows 7 and XP menus: a program's right-click menu (Open, Pin to
      Start menu / Unpin from Start menu, and Remove from this list for
      the frequently used ones) writes `start-pinned` / `start-frequent`.
      The Start menu closes afterwards (Windows 7 keeps it open). Tested
      headless: unpinning a default, pinning a search result, removing a
      frequent program.
- [x] Windows 7 menu: jump lists (the ▸ beside a program; opens beside the
      row rather than in the right column, and not by keyboard yet).
- [x] Windows 2000 menu: Documents lists My Documents, the Home folder and
      the 15 most recent files (recently-used.xbel) in name order, rebuilt
      as the menu opens; a menu taller than the screen continues in more
      columns. Tested at 2x (360px tall): System Tools in two columns,
      hover in both.
- [x] Menus: one highlight at a time when moving quickly (the item whose
      submenu waits to close no longer stays lit). The Windows 2000 banner
      picks no item, as in Windows.
- [x] Greyed-out items: Search (xd-search: name, wildcards, text inside
      files, where to look; no date/type/size options), Help (xd-help),
      Devices and Printers, Default Programs (xd-defaults, GIO defaults
      for six kinds), Switch User (the display manager's SwitchToGreeter or
      GDM's CreateTransientDisplay; greyed without one). Windows 7 search
      finds Control Panel applets; Computer opens computer:///. Tested
      headless, Switch User against a mock.
- [x] Start menu keyboard navigation (XP and Windows 7): arrows within and
      across columns, Tab round them, Enter, Esc. Tested headless; Windows
      7's exact Left/Right/Tab behaviour is a best guess.
- [x] Switch User: xd-lock follows logind's Unlock (sent by the display
      manager after its greeter) and Lock, so returning asks once; XP's Log
      Off box in Luna has Switch User. A logind Lock (loginctl
      lock-session, a lid or power key set to lock) starts xd-lock through
      the shell when it isn't running; the box's big buttons can't take
      keyboard focus.
- [?] Windows 7 menu: Recent Items, the Shut Down ▸ submenu, Run, the
      frequently-used list showing launched apps.
- [?] Quick Launch buttons launching apps; Show Desktop's second click
      restoring windows.
- [?] Wallpaper modes other than Fill (Center, Tile, Stretch, Fit).
- [~] Notifications (notifyd.c, notifyfly.c): our own
      org.freedesktop.Notifications server, as XP's balloons from a bell
      in the notification area, queued one at a time, with actions (as
      links), urgency, replaces_id, icons and pictures, the scheme's
      Notification sound. History of the missed ones (kept across logons)
      from the bell, with Clear all and Do Not Disturb; full-screen
      windows hold balloons back. Tested headless (Luna). Not done:
      Windows 7's own balloon look (Aero draws XP's), the sound-name and
      sound-file hints (the scheme's sound plays), ActivationToken (so a
      program raised by an action may not get focus), keyboard access to
      the history's rows. Untested with real programs (Firefox, Thunderbird,
      chat clients, the portal's Notification backend).
- [ ] Snipping Tool (xd-snip): Windows 7's, through the compositor's
      capture (screencopy): free-form, rectangular, window and full-screen
      snips, a delay, then a markup window (pen, highlighter, eraser),
      Copy, Save As (PNG/JPEG), Print Screen bindings. Screen recording:
      the same capture (or the ScreenCast portal) into PipeWire and
      GStreamer (WebM/MP4), with the microphone or the computer's sound,
      started from the Snipping Tool and stopped from a tray icon.

## Theme engine

- [x] All 22 of XP's classic colour schemes (data/schemes/*.ini, XP's
      names), every colour from a published table of XP's system colours
      and cross-checked against ReactOS's copy (cited in each file).
- [ ] Large/Extra Large scheme metrics (and Pumpkin (large)'s sizes): no
      reliable source found; the Font size setting scales instead.
- [~] Luna matched to a Windows XP screenshot (tools/luna-measure.py makes
      src/theme/luna_data.h): the active and inactive frame (caption,
      borders, corner cut-outs), caption buttons (faces within ~3% RMS),
      taskbar, Start button, task buttons, tray, push buttons and check
      boxes match at 100%. Text uses DejaVu Sans Condensed for Tahoma,
      Source Sans 3 for Trebuchet MS (captions) and Roboto Condensed for
      Franklin Gothic (Start), unless the real fonts are installed. Not matched:
      the hover, pressed and focus states and the Restore and Help glyphs
      (not in the reference, so designed), radio buttons, tabs, scroll
      bars, group boxes, menus and the GTK theme's Luna section, which
      still uses the older approximations.
- [~] Luna colour variants Silver, Olive Green and Energy Blue (Royale),
      each measured from a screenshot (tools/luna-measure.py --variant
      silver|olive|energyblue; sources cited there): caption, frame,
      caption buttons, taskbar, Start button, task buttons, tray; the XP
      Start menu takes each variant's colours (Energy Blue's measured).
      Derived, with no reference: hot/pressed caption buttons; Olive's and
      Energy Blue's inactive windows; Silver's pressed task button; Silver's
      and Olive's Start menus (Blue's recoloured). Olive's reference is a
      pre-release build (2485); Silver's frames are fitted from a 3px
      dialog frame; Energy Blue's Start menu reference is a 256-colour
      image. Blue is unchanged (pixel-compared before and after).
- [~] Aero (Windows 7), style_aero.c: translucent sky-blue glass frames
      with a sheen and faint streaks, rounded corners, the dark outline
      and light inner edge, the grouped caption strip with a red Close,
      the title (Segoe UI, Open Sans standing in) on a soft white glow;
      a glass taskbar with a glossy round Start orb and glass task
      buttons; Windows 7's light buttons, check boxes, menus and dialog
      colours. Blur, Windows 7's taskbar and colourisation: see above.
      Not yet: Aero's GTK theme beyond the shared rules.
- [~] Whistler is the "Watercolor" look, matched against screenshots
      (frame, caption mosaic, caption buttons, buttons, fields, tabs, scroll
      bars, taskbar). The Start menus use Watercolor's colours. Menus, group
      boxes and inactive colours are guesses.
- [x] Fonts without Wine: the closest packaged fonts stand in for Windows'
      (src/theme/scheme.h). Measured against Wine's Tahoma at 8pt (11px)
      from outlines, among 20 sans families packaged by both Arch and
      Debian: DejaVu Sans Condensed matches its text width to 1% (regular
      and bold) with the best glyph overlap (0.77 regular, 0.87 bold;
      next 0.72 and 0.78). Trebuchet MS and Franklin Gothic were compared
      by eye (Source Sans 3, Roboto Condensed). System fonts, so third-party
      GTK apps get them too.
- [ ] Tahoma at 8pt on XP is crisp bitmaps; DejaVu is drawn from hinted
      outlines.

### Theme packages and Windows visual styles (docs/THEMES.md)

- [x] The .xdtheme format (zip or folder: theme.xml, styles/VARIANT.xml in
      uxtheme's classes, parts, states and properties, PNG images) and
      xd-theme-import converting XP, Vista and 7 .msstyles and .theme
      files (python-pefile). data/theme/uxtheme.ini: uxtheme's documented
      names. Tried on Luna, Royale, the five WE styles, Vista's and 7's Aero.
- [x] The renderer (src/theme/pkg.c): lookups with uxtheme's fallbacks,
      DrawThemeBackground's image strips, 9-grid, TrueSize, ImageFileN by
      DPI or size, image glyphs, BorderFill, atlas rectangles; the
      package's [SysMetrics] over the scheme; window frames and caption
      buttons; push, check and radio buttons, group boxes, text fields,
      drop-down buttons, vertical scroll bars, tabs.
- [x] Image scaling for high DPI: Nearest, Integer, Smooth, Scale2x/3x/4x,
      2xSaI, Super2xSaI, SuperEagle (our own, from the published
      descriptions), whole-number scaling, per frame so states don't bleed.
- [x] Choosing a package: [Appearance] ThemePackage, ThemeVariant,
      ImageScaling, IntegerScaling in desktop.ini (instead of today's
      $XD_THEME_PACKAGE, $XD_THEME_VARIANT, $XD_IMAGE_FILTER), packages in
      ~/.local/share/xeondesktop/themes and the system's share folder, the
      compositor, shell and apps following changes live. Display Properties:
      packages in "Windows and buttons", their variants in "Color scheme"
      and "Font size", Browse... to import a .msstyles or .theme (running
      xd-theme-import), "Scale theme images" in Effects. Done; no package
      while High Contrast is on. Not done: the desktop's background colour
      from a package; re-importing under the same name reloads only on the
      next desktop.ini change.
- [x] GTK and Qt programs: the generated GTK theme drawn from the package
      (CSS border-image with its images), the Qt palette from its colours.
      Not right yet: scroll thumb grippers and some glyphs can sit half a
      pixel off (CSS centring), GTK's spin buttons sit side by side, only
      tabs on top are styled.
- [x] The rest of the controls: horizontal scroll bars and size boxes,
      progress bars, sliders, spin buttons, list view headers, tree view
      glyphs, toolbars and rebars, status bars, tooltips, Vista/7 menus
      (Menu), the taskbar's Quick Launch buttons and grips. Not done: tool
      windows' small captions (xd_frame has no tool window flag), MDI
      buttons (nothing draws them). Progress, spin, header, tree glyph,
      status bar and menu bar have no caller outside xd-themeview yet.
- [x] Vista's and 7's images are premultiplied: xd-theme-import makes
      them straight (through cairo), or translucent parts came out dark.
- [~] Vista and 7 frames: DWMWindow's caption buttons, glows, glyphs,
      frame edges and glass reflections named (deduced) and drawn with the
      glass; Basic from Window; frames sized from the metrics, as Windows
      does. The DWM shadow pieces aren't used (the compositor draws its
      own). The glass path is untested on a GPU.
- [x] Taskbar and Start menus from packages: TaskBar, TaskBand,
      TaskBand2 (7's buttons and progress colours), TrayNotify*, the
      clock, Start button, XP's StartPanel, 7's StartPanelPriv, and our
      ClassicStartMenu for the Windows 2000 one (and our JumpList). Not
      done: the group menu isn't styled (FlashButtonGroupMenu).
- [x] Programs' progress, counts and urgency on the taskbar, from Unity's
      LauncherEntry over D-Bus (Firefox, Thunderbird, Chromium, KDE): 7's
      green bar, a count badge on the icon, urgent flashing (on every
      taskbar). Not done: LauncherEntry's Query after a shell restart;
      progress is per program, not per window.
- [x] Explorer's panes: XP's task pane (ExplorerBar, and shellstyle.dll's
      stylesheet imported as shell/VARIANT.xml), 7's Navigation,
      CommandModule, PreviewPane, ReadingPane, the address and search bars.
      xd-files has a task pane now (in place of the Folders tree when
      Folders is off). Not done: keyboard navigation in the pane, the
      "blockade" banners, 7's Navigation back/forward buttons.
- [x] Folder Options' "Show common tasks in folders" / "Use Windows classic
      folders" ([Folders] CommonTasks): the task pane by default as in XP,
      View > Explorer Bar > Folders, the tree for xd-files with no folder
      or --explore (XP's Windows Explorer and Explore). Not done: XP's
      other Explorer bars (Search, Favorites, Media, History).
- [x] Proper names for the undocumented shell classes' parts, deduced
      from their images and use, marked as deduced in uxtheme.ini:
      AddressBand, AltTab, BreadcrumbBar, CommandModule, DragDrop,
      DWMWindow, InfoBar, ItemsView, MonthCal, Navigation, PreviewPane,
      SearchBox, SearchHome, StartPanelPriv, TaskBand2,
      TaskbandExtendedUI, TrayNotifyFlyout, TryHarder and the rest the
      importer leaves numbered. Named so far (docs/THEMES.md): DWMWindow's
      buttons and glyphs, the parts of TaskBand2, TaskbandExtendedUI,
      StartPanel, StartPanelPriv, CommandModule, PreviewPane, ReadingPane,
      InfoBar and SearchHome that we draw, AltTab, TextGlow, ProperTree,
      TrayNotifyFlyout, HelpSearchBox; and the classes Wine documents
      (Link, EmptyMarkup, Static, MonthCal) in the regular sections. Left
      numbered, with reasons in uxtheme.ini: DWM's other edge sets and
      animations, ItemsView, TryHarder, DropListControl, SearchEditBox,
      QueryBuilder.
- [x] Alt+Tab's window (there was none: Alt+Tab only raised windows):
      Windows 2000/XP's box in every style, 7's from a package's AltTab.
      Not done: hover (HotItem), 7's live thumbnails in it.
- [x] 7's clock flyout: the unified taskbar's clock, clicked, shows
      today's date, a month calendar (from the locale's first weekday;
      arrows, Page Up/Down, Home) and an analog clock, with "Change date
      and time settings..." below; XP's taskbar keeps the double-click.
      Aero's from memory of 7, the other styles' as XP's MonthCal (today
      ringed in red). UI test clock_flyout. Not done: 7's zoom out to the
      year and decade views (clicking the month), additional clocks.
- [ ] 7's notification area overflow flyout (TrayNotifyFlyout): the
      unified taskbar still folds quiet icons behind XP's chevron.
- [~] What a .theme brings besides the style: Display Properties' Themes
      tab (XP's first tab: Windows XP, Windows Classic, packages bringing
      more than a style; Save As..., Delete, Browse...) applies the style,
      wallpaper, sound scheme and pointers together; xd-theme-import
      brings a .theme's sounds and pointers when they sit beside it.
      Pointers need win2xcur (python-win2xcur, AUR, or pip) and are
      untested; a .theme without a visual style (Windows Classic.theme)
      can't be imported yet; 7's named system sound schemes are ignored.
- [~] Our built-in styles matched to the real ones: render each part from
      the imported packages and from our code, pixel-diff them, and fix
      Luna (Blue, Silver, Olive Green), Energy Blue (Royale), Aero (7) and
      their DPI sizes. Done with tools/xd-theme-compare and measured numbers only
      (controls-measure.py: sizes, radii, edge colours, a few gradient
      stops; no pixels): Luna's four variants' controls and caption
      buttons, 7's controls. Left: XP's caption buttons' diagonal shading
      and 7's glass glow (two-dimensional, not in our measured shapes),
      Luna's Large and Extra Large Fonts sizes, Whistler and Classic at
      high DPI.
- [x] Tests: unit tests of the loader and renderer with a package made in
      the test (our own images), and a UI scenario (theme_package) drawing
      frames and controls from one.

## Dialogs and libxdui

- [ ] libxdui's own text boxes (Run...) don't use text-input-v3 yet (the
      compositor relays it for GTK and Qt programs: see Linux settings), no
      accessibility (AT-SPI), no drag and drop in libxdui (radio buttons and tooltips exist, unused
      so far). libxdui windows can't be owned by another (Monitor Settings
      gets its own taskbar button). The slider (trackbar) is drawn
      Classic-style in every theme. A scroll-thumb drag stops when the
      pointer leaves the program's own windows.
- [x] Run: Browse... opens the GTK chooser (xd-choose-file) with XP's
      Programs filter and puts the choice in the Open box.
- [~] Display Properties:
  - Screen Saver tab: saver list with a live preview, Preview, Password
    protected, Wait, and "Turn off monitor" (instead of Power Options).
    Tested. Settings opens the chosen saver's own options.
  - Settings tab: monitor arrangement by dragging (snaps edge to edge),
    resolution slider, refresh rate, scale, orientation, extending onto a
    monitor, and Windows' 15-second "keep these settings?" check that
    reverts by itself. Tested with two headless outputs: scale change,
    revert on timeout, dragging a monitor underneath and keeping it. Not
    tested with real modes or rotation. Identify shows each monitor's
    number on it (the shell's org.xeondesktop.Shell.Identify). No
    Advanced.
  - Background: Browse... (GTK file chooser). Tested.
  - Effects: menu and tooltip transitions (Fade/Scroll, done by the
    compositor for the shell's popups, Windows 7 thumbnails and the
    volume flyout too), large icons (desktop and XP Start menu), outline
    dragging (a checkered frame; Aero's blur isn't clipped during the
    animation), hiding keyboard cues. Not yet: "Show shadows under
    menus".
  - Appearance: Advanced (Windows XP's Advanced Appearance: the clickable
    preview, 18 items, sizes, two colours, fonts; the 20-colour palette
    and the Color dialog), saved as desktop.ini's [CustomScheme]. Icon
    Size, Icon Spacing and Palette Title are saved but not drawn with
    yet; colour buttons take no keyboard focus.
  - Linux tab: the icon theme and the GTK and Qt program styles.
- [~] Display Properties' Apply (tested: screen saver settings, display
      settings with the keep/revert check). Untested: OK with unapplied
      display changes (it applies, asks, then closes on Yes).
- [?] List box: mouse selection, scroll bar arrows/track/thumb, wheel.
- [?] Clipboard copy/cut/paste in edit fields; key repeat.
- [?] Run's history drop-down (keyboard history selection was tested).

## File manager (xd-files)

- [x] File operations (fileops.c, on a worker thread with GIO): copy, move
      (a rename on the same file system), shortcuts (symbolic links,
      "Shortcut to x"), Recycle Bin and Shift+Delete, with XP's "Copying..."
      dialog (flying paper, From 'a' to 'b', green blocks, Seconds
      Remaining), "Confirm File Replace" / "Confirm Folder Replace" (Yes,
      Yes to All, No, Cancel; folders merge), "Copy of x" beside the
      original, and Skip/Cancel on errors. Tested: replacing, Cancel,
      "Copy of", copying all of /usr/share/doc (221 MB, 9,000 files:
      identical, symlinks kept), cut and paste kept the inode, Delete with
      "can't be recycled: delete permanently?" (GLib has no Recycle Bin on
      /tmp).
- [x] Clipboard: Cut (faded until pasted), Copy, Paste, Paste Shortcut,
      as x-special/gnome-copied-files, text/uri-list and text, so other
      file managers can paste them and the other way round. A cut is
      cleared once pasted.
- [x] Drag and drop into a folder or onto a folder item (highlighted):
      move on the same file system, else copy; Ctrl copies, Shift moves,
      Ctrl+Shift makes a shortcut (the compositor passes modifiers to the
      client during drags, which wlroots doesn't). Wayland has no "link"
      action, so a shortcut travels as a copy.
- [x] File > New > Folder / Text Document, Rename (F2) in place, Create
      Shortcut, Delete; Edit > Copy To Folder... / Move To Folder...;
      context menus with Cut, Copy, Paste (into a selected folder), Create
      Shortcut, Delete, Rename, New. Tested: New Folder named in place,
      renaming onto an existing name (XP's message, keeps editing), Escape.
- [?] Untested: the real Recycle Bin (sending files from the home folder),
      moves between file systems, Copy To / Move To Folder, pasting
      between xd-files and other file managers, permission errors and Skip.
- [x] XP's shortcut arrow on symbolic links and launchers, in xd-files
      and on the desktop (xd_draw_shortcut_arrow).
- [x] Launchers (.desktop files) show their program's name and icon, typed
      "Shortcut", as Windows shows a .lnk; renaming one changes its Name
      (every translation) and keeps the file. Tested from the desktop.
- [x] Edit > Undo (Ctrl+Z, the last 10 renames, copies, moves and deletes
      to the Recycle Bin; per window, not kept), Send To (Compressed
      (zipped) Folder with libarchive, Desktop, Mail Recipient for
      Thunderbird/Evolution/KMail, My Documents, removable drives), a
      modifier pressed mid-drag applies at once. Untested: Mail Recipient,
      Send To a removable drive.
- [x] Extract Here and Extract to "name" on archives' right-click menus
      (libarchive's formats; "Extract Each to Its Own Folder" for
      several), with the progress dialog, Confirm Replace questions and
      Undo Extract. Entries outside the folder, device files and
      set-user-ID bits are left out. Not done: password-protected
      archives (they fail with libarchive's message).
- [x] Favorites menu (GTK bookmarks, shared with file choosers), the Search
      button and F3 (xd-search in the current folder), "My Network Places"
      and GIO's names as titles.
- [~] Properties dialog. Tested: General for a file, Permissions (ticking
      Group Write and Apply changed 644 to 664), Details, and the size and
      contents count for a folder plus a file (matches find/du exactly).
- [?] Properties, untested: renaming and Hidden (renames to/from a dot
      name), Read-only, Advanced (chattr) apply, Change... (default app),
      changing the group, "Apply changes to all files and folders inside",
      tri-state check boxes with several mixed items, the error paths.
- [~] Properties: a Shortcut tab for .desktop files (a user copy when the
      file isn't writable), a picture's Summary (dimensions). Not yet: audio
      tags (no tag library), ACLs, SELinux context.
- [x] `xd-thumbnailer`: missing thumbnails made in a bubblewrap + seccomp
      sandbox (no network, only the input file, CPU/memory/time limits;
      only raw pixels come out), images by gdk-pixbuf, other types by
      installed .thumbnailer programs; visible items first. Local files only.
- [x] Details view: column resizing (double-click a divider to fit),
      reordering, and the header's column menu (Date Created/Accessed,
      Attributes, Owner), remembered per folder. No Dimensions column (GIO
      has no image size).
- [x] Rubber-band selection (XP's translucent rectangle; Ctrl toggles from
      the selection it started with; the view scrolls past the edges),
      View ▸ Arrange Icons by (and on the background's menu), and the
      status bar's "(Disk free space: ...)". Tested headless.
- [x] Each folder remembers its view and sort (the "folder-views" state
      file, the 400 most recent, as XP keeps), saved on leaving it or
      closing the window; folders left at the defaults aren't stored.
      Tested: Details in one folder, Icons in its parent, Back restores it.
- [x] Arrange Icons by ▸ Auto Arrange and Align to Grid; free placement
      remembered per folder (folder-positions).
- [x] My Computer (`computer:///`, places.c): the file system, the user's
      Documents and every mount from GIO's volume monitor (live), as Local
      Disk / Removable Disk / CD Drive / Network Drive with total and free
      space (Details: Total Size, Free Space; Tiles: "x free of y"). Up
      from a drive goes to My Computer. Tested headless: the root disk and
      the VirtualBox Guest Additions CD.
- [x] Recycle Bin (`trash:///`): the freedesktop.org trash in
      $XDG_DATA_HOME/Trash, read with GKeyFile (no gvfs needed), under the
      original names with Original Location and Date Deleted columns;
      Restore (recreating the folder), Delete (permanently), Empty Recycle
      Bin, drops into it send files to it; items open their Properties.
      Tested: Restore, Empty Recycle Bin, .trashinfo files tidied.
- [x] My Computer in XP's groups (Files Stored on This Computer, Hard
      Disk Drives, Devices with Removable Storage, Network Drives), in the
      icon views; Arrange Icons by > Show in Groups for any folder (by
      initial, type, Tiny/Small/Medium/Large/Huge, or how long ago),
      remembered per folder. Volumes not mounted yet are listed and mount
      when opened (GTK's mount operation asks for passwords); Eject on
      drives that can (or unmount). The Recycle Bin also lists other
      drives' trashes (.Trash-UID, .Trash/UID; relative paths from the
      drive's top), and Restore asks before replacing a file (Yes, Yes
      to All, No, Cancel). Tested headless: the groups, Show in Groups by
      type, Eject offered for the CD (not pressed), Restore replacing, a
      second drive's trash (a bind mount) listed and restored. Untested:
      mounting a volume and ejecting (no spare disk here).
- [x] My Computer: drive Properties with XP's pie chart; groups in Details
      (List view can't group, as in XP). Renaming a drive (UDisks2
      SetLabel), Disk Cleanup (the user's own files only), the Tools tab
      (Check Now through UDisks2, Defragment's "not needed", Backup with a
      backup program) and the Hardware tab; tested against dbusmock's
      UDisks2. Untested: a successful check or repair, real polkit
      prompts, real disks.
- [x] Tabs (Windows 11's): Ctrl+T/W, Ctrl+Tab, Ctrl+1..9, middle click and
      "Open in new tab", per-tab history/view/selection, drag to reorder,
      and Folder Options' "Open folders in new tabs instead of new
      windows". Tested headless in all four styles. Unsure: whether
      Windows 11 opens middle-clicked tabs in the background.
- [x] The first arrow key in a fresh folder selected the second item.
- [?] Folders tree clicks, address bar typing, the Views menu, the
      thumbnail slider, Details header sorting, Show Hidden Files, opening
      files with the default app, live updates, very large folders.
- [ ] Previous Versions: a Properties tab and "Restore previous
      versions" for files and folders, when the file system keeps
      snapshots: Snapper on btrfs (`.snapshots/N/snapshot`), Timeshift's
      btrfs or rsync snapshots, ZFS (`.zfs/snapshot`); each version's
      date with Open, Copy... and Restore. Not shown where there are none.

## GTK3 theme

- [~] Generated GTK3 theme (`$XDG_DATA_HOME/themes/XeonDesktop`), rewritten
      by the compositor at start and on every appearance change. Our GTK
      apps reload it live. Covers buttons, entries, check/radio, menus,
      scroll bars, tabs, tree headers, panes, scales, tooltips, toolbars.
- [x] XP push buttons (Luna, Watercolor): GTK uses the theme engine's own
      button, saved as PNGs at 1x and 2x, as a nine-slice border image, so
      GTK and libxdui buttons match. Compared with the XP screenshot: the
      same 75x23 footprint (73x21 inside the glow), border, corners, face
      gradient and text baseline; hover, pressed, default and disabled
      states. Font sizes are rounded to whole pixels as GDI does (8pt =
      11px). Left: FreeType draws Tahoma a pixel smaller in places than
      Windows does (e.g. "OK" is 13x8 instead of 14x9).
- [x] Submenu arrows (Windows' 4x7 arrow, white when highlighted), spin
      buttons (narrow, Windows' small arrows instead of + and -; GTK keeps
      them side by side), info bars (the tooltip colour), switches (a
      sunken track and a button-like thumb), progress bars (Windows 2000's
      blocks in Classic; XP's green blocks in Luna). Checked with a GTK
      test window in Classic and Luna.
- [x] Popovers (other programs' menus) look like Windows menus: square,
      the menu colour and border, highlighted rows, the submenu arrow.
      Combo box pop-ups are GTK menus, already styled.
- [x] GtkFileChooser's sidebar (a list) and path bar (flat buttons); combo
      pop-ups are list boxes. Other apps' header-bar dialogs keep a GNOME
      layout (GDK forces gtk-dialogs-use-header on Wayland).
- [~] Other apps' GtkMessageDialogs: Windows' dialog frame, Yes before No;
      buttons still stretched (set in GTK's code), title centred, no close
      button, strip doubled under AlwaysDrawFrames.
- [x] The selected tab overlaps its neighbours by 2px (box shadows).
- [?] Luna tabs, group boxes, combo boxes and check glyphs: seen in the
      System Properties and Run As screenshots and look right, but not
      compared pixel for pixel with XP like the buttons were.
- [?] Text everywhere grew from 10.67px to 11px (whole pixels, as GDI).
      Display Properties, System Properties, Lock Computer and Run As were
      looked at afterwards; the other libxdui dialogs (Run, Shut Down,
      Taskbar and Start Menu Properties) and the Start menus weren't
      re-checked for clipped or crowded labels.
- [?] Luna/Whistler visuals beyond the file manager's window, and every
      scheme other than Windows Standard: only checked to parse (unit test),
      not looked at.

- [x] Third-party GTK3 apps follow appearance changes: a real session
      switches GSettings' gtk-theme between XeonDesktop and XeonDesktop-alt
      (only while ours is used and the key is ours or empty). Sandboxed
      programs and GTK 4 don't follow.
- [ ] GTK4/libadwaita apps aren't themed (libadwaita ignores themes).

- [x] Display Properties > Effects: icon theme (IconTheme=; the setting
      wins over XD_ICON_THEME) and GTK theme (GtkTheme=, written to
      gtk-3.0/gtk-4.0 settings.ini and GSettings only when changed there).
      The shell, title bars, xd-files and our GTK apps follow it live;
      libxdui apps and the lock screen at their next start.

- [x] Qt programs: QT_QPA_PLATFORMTHEME=gtk3 with our palette
      (QT_GUI_GTK_JSON, written beside the generated GTK theme), and
      Display Properties' Program style (Qt) (QtStyle=; Automatic: Windows
      for Classic, Fusion otherwise); the user's own QT_* variables win.
- [ ] Qt: file dialogs are GTK's chooser with a GNOME header bar (GDK on
      Wayland forces it); Fusion can't draw Luna's gradients; Qt 5 gets no
      palette file; GtkTheme/IconTheme are re-read only by Qt programs
      started after a change. The icon theme is written to GSettings at
      every real login, which other desktops sharing that key also see.
      Our GTK apps unset GTK_THEME, so programs started from xd-files fall
      back to the GSettings GTK theme.
- [ ] Our own file dialogs: an xdg-desktop-portal FileChooser backend
      (xdg-desktop-portal-xeondesktop) with XP's and 7's Open and Save As
      dialogs, from xd-files' folder view (Places bar, Look in, file
      types), used by Flatpaks, GTK4, Firefox and Chromium, and Qt with
      QT_QPA_PLATFORMTHEME=xdgdesktopportal; GTK3 programs with
      GtkFileChooserNative through GTK_USE_PORTAL=1. Plain
      GtkFileChooserDialog stays GTK's (themed).
- [ ] GTK4/libadwaita: a generated ~/.config/gtk-4.0/gtk.css from the
      theme package (colours, fonts, buttons, as far as libadwaita lets
      it), written only while ours is in use.
- [ ] Qt: a generated Kvantum theme (SVG) from the theme package, so Qt
      programs get Luna's and Aero's gradients and Qt 5 a palette.

## Task Manager (xd-taskmgr)

- [x] "Not Responding": the compositor pings every Wayland window every 2s
      (5s timeout, Windows' hung-window time); a hung one gets
      " (Not Responding)" after its title on the frame, the taskbar and the
      foreign-toplevel title, and Task Manager's Status says so, until it
      answers again. Tested with a program that froze its main loop for
      25s. X11 windows too, when they list _NET_WM_PING (wlroots' X11
      ping timeout is its own 10s); tested with the same program under
      GDK_BACKEND=x11. Closing a window that isn't responding opens
      Windows' End Program box (xd-endtask: End Now kills the program,
      checking it's still the same process; Cancel). Tested: End Now on
      a frozen program. A hung window that draws its own title bar
      (Firefox, GNOME programs) gets our frame meanwhile, as XP's ghost
      window, whose Close offers End Program; it goes back as it was when
      the program answers (UI test: hung_csd_window_ghost_frame). A
      window that hangs after a close request gets the box after 5s
      (UI test end_program_after_close); one that asks "Save changes?"
      doesn't.
- [x] Options > Always On Top (saved): xd-window-arranger-v1 version 5's
      set_always_on_top puts the client's own window (and its dialogs) in
      a layer above ordinary windows, below the taskbar. Tested: Task
      Manager stays in front of a newly focused window.
- [x] The CPU meter in the notification area while Task Manager runs
      (trayicon.c: a StatusNotifierItem with green LED rows and "CPU Usage:
      N%" as its tooltip; a click restores the window; its dbusmenu has
      Restore, Always On Top and Close Task Manager). Options > Hide When
      Minimized (on by default, as in Windows; [TaskManager]
      HideWhenMinimized) takes the minimized window off the taskbar into
      the icon; it's greyed out, and a hidden window comes back, when
      there's no notification area. Tested headless: the icon, Win+Down
      hiding the window, a click restoring it, the menu.
- [x] Windows menu: Tile Horizontally/Vertically and Cascade arrange the
      windows selected on the Applications tab (several with Ctrl/Shift;
      End Task, Minimize and Maximize act on all of them). The compositor
      offers xd-window-arranger-v1 (version 4: mark + arrange_marked) to
      the shell and to the xd-taskmgr installed beside it. Tested: two
      windows tiled vertically, the shell still gets its events.
- [x] Window icons: title bars and the taskbar use the desktop file's
      icon, else one named after the app id, else the generic program icon
      (the compositor loads them in a thread).
- [ ] Handles has no Linux equivalent; Open Files is shown instead, and is
      0 for other users' processes (as is I/O).
- [x] Peak Mem Usage is the lifetime peak (VmHWM).
- [x] XP's Networking tab (per-adapter graphs, sysfs) and Users tab
      (logind sessions, Logoff; Disconnect for your own session; Send
      Message greyed).
- Tested: Ctrl+Alt+Delete and Ctrl+Shift+Esc opening it, Applications
  listing windows and End Task closing one, Processes (type-ahead select,
  Delete, warning, Yes killed the process), Performance meters, graphs
  and totals, KB/MB/GB units, Ctrl+Tab.
- [?] Untested: pressing Ctrl+Alt+Delete while it's open (should focus
      it), Switch To / Bring To Front / Minimize / Maximize, Large and Small
      Icons, End Process Tree, Set Priority, Set Affinity, Select Columns
      and the optional columns, One Graph Per CPU, Show Kernel Times,
      Update Speed, New Task, Minimize On Use, saved settings.

## Control Panel

- [x] The `control:` folder in xd-files (Category View and Classic View,
      remembered; applets found by their .desktop files' category keys;
      deleting/renaming them refused), `xd-control [NAME]`, and the Start
      menus' Control Panel listing the applets (2000: Settings > Control
      Panel cascades and opens the folder when clicked). Tested headless:
      both views, the categories, the three Start menus, the address bar.
- [x] Applets added (each tested headless; writes to system services only
      against python-dbusmock): Mouse (`[Mouse]`, applied to libinput
      pointers by the compositor, cursor theme/size live), Keyboard (repeat,
      cursor blink, up to 4 layouts, XKB options from libxkbregistry),
      Accessibility Options (StickyKeys, BounceKeys, High Contrast), Sounds
      and Audio Devices (libpulse: volume, default devices; XP's sound
      scheme with libcanberra), Power Options (`[Power]`: monitor off,
      standby and hibernate timers through logind, the battery icon, lock
      on resume; the lid, power and sleep buttons per user: the shell takes
      logind's block inhibitors for the ones changed and acts itself, the
      lid doing nothing when docked; UI test power_buttons on dbusmock's
      logind, the real lid switch untested), Regional and
      Language Options (your LC_* formats in environment.d with samples;
      the system locale and keyboard through localed), User Accounts (XP's
      task pages over AccountsService: picture to ~/.face, name, password
      via xd-security, create/delete/type through polkit), Folder Options
      (`[Folders]`, obeyed live by xd-files: own window, single click, hidden
      files, extensions, full paths, remembered views; File Types with
      Change...), Fonts (fontconfig list, XP's viewer, install to
      ~/.local/share/fonts, delete user fonts to the Recycle Bin). Merged and
      checked together: every applet in its category, saving from one keeps
      the others' sections.
- [ ] Applets: untested on real hardware: libinput settings (left-handed,
      speed, acceleration), a real keyboard in a second layout, real
      suspend/hibernate and lock on resume, real sound cards and default
      device switches, real localed/AccountsService/polkit prompts, a
      changed password at logon, a logon picking up the formats file.
- [ ] Applets: not done: UPS (Linux uses NUT/apcupsd; UPower lists USB UPSes
      as batteries); Customize Regional Options' fields glibc can't express
      (list separator, two-digit years, long date...); limited users see
      administrator tasks in User Accounts; other GTK apps don't follow the
      double-click time or caret blink.
- [x] Power Options' Alarms and Power Meter tabs (the shell's alarms follow
      them); Accessibility's Sound tab (SoundSentry flashes), status icons
      in the tray, automatic reset after idle. Untested on a real laptop.
- [x] Sound events: window open/close/minimize/maximize/restore (the
      compositor), menu popup and command, device connect/disconnect, low
      and critical battery (the shell), message box sounds, Empty Recycle
      Bin, Windows Logoff/Exit, Default Beep (xdg-system-bell). Played off
      the main loops; XD_SOUND_TRACE logs instead of playing.
- [x] Accessibility: SlowKeys, ToggleKeys, MouseKeys, ClickLock, StickyKeys'
      options, the four activation shortcuts with XP's confirmation box,
      and the applets' Settings dialogs. Tested headless.
- [x] Power Options' Plugged in / Running on batteries timers and scheme
      Save As/Delete; Regional Options' Customize (a localedef-compiled
      locale in LOCPATH, set through environment.d); File Types New and
      Delete (the user's shared-mime-info database); Fonts' Print.
- [ ] Untested for real: ToggleKeys' tone with the real lock state
      (xd-inject sends none), battery alarms and timers on a laptop,
      device sounds, a logon applying customised formats.
- [x] Second round of applets (each tested headless; every write only
      against mocks or private instances): Network Connections (libnm:
      XP's groups and status texts, Status/TCP/IP/Wireless dialogs, XP SP2's
      wireless chooser, New Connection wizard; the tray opens it), Bluetooth
      Devices (BlueZ: XP SP2's Add wizard and passkeys, an Agent1 for PIN
      dialogs, Options/Hardware; the tray menu opens it), Game Controllers
      (udev + libevdev, read-only test page), Administrative Tools (Services
      over systemd, Event Viewer over the journal, Disk Management over
      UDisks2 as a full partition manager with the system's volumes
      protected), Scheduled Tasks (XP's wizard over systemd user timers),
      Printers and Faxes (CUPS: queues, preferences, Add Printer wizard,
      password box), Scanners and Cameras (SANE wizard with preview,
      gphoto2 photo copying), Add or Remove Programs (PackageKit and
      Flatpak: remove with a dry run naming what else goes, search and
      install, package files, groups).
- [ ] Second-round applets untested for real: Disk Management's writes on a
      real disk (try a spare virtual disk first: initialize, create,
      format, resize, delete, a full format, a startup mount point in
      /etc/fstab and booting with it), real polkit prompts for services,
      packages and printers, automatic logon with a real GDM
      (`xd-control userpasswords2`), a next-start boot entry with a real
      systemd-boot, real pairing, a real controller's test page, real
      printers/scanners/cameras, DNS-SD printer discovery, a real systemd
      user manager running scheduled tasks.
- [~] Session: a real login hands the user manager WAYLAND_DISPLAY,
      DISPLAY, XDG_* and the session bus, and starts
      xeondesktop-session.target (BindsTo graphical-session.target), which
      xeondesktop-session stops when the compositor exits; so "When I log
      on" tasks run and scheduled programs can open windows. Tested against
      a mock user manager; untested with a real login.
- [x] Add or Remove Programs: Last Used On (shell launches and documents),
      each program's source (pacman/APT repository, Flatpak remote,
      AppImage, not from a package) and Sort by: Source.
- [~] **System Properties** (`xd-sysdm`, Windows 2000's layout, Linux
      inside): General (distribution, kernel, owner, machine ID, CPU,
      firmware, RAM); Network Identification (host name and description
      through systemd-hostnamed, domain or Samba workgroup, computer type);
      Hardware (Kernel Modules, Driver Signing = Secure Boot / module
      signature enforcement / lockdown / taint, Device Manager from udev
      with General/Driver/Details per device, Power Profiles through
      power-profiles-daemon); User Profiles (home folders with their size;
      Delete removes the account through AccountsService; Change Type and
      Copy To stay greyed, with no Linux meaning);
      Advanced (Performance = scheduler and swap/zram, Environment
      Variables in `~/.config/environment.d`, Startup and Recovery = the
      boot loader's entries from EFI variables, the next start's entry
      through logind's SetRebootToBootLoaderEntry where it can (the
      permanent default needs an administrator), panic and core dump
      handling). Tested: every tab and dialog opens and shows the right
      data; adding a user environment variable. **Untested: renaming the
      computer, its description and changing the power profile** (they go
      through polkit; see the agent below). Device Manager: no "by
      connection" view, no disable/uninstall/update driver. User
      Profiles: Delete, Change Type and Copy To are greyed
      (administrator jobs).
- [~] **polkit authentication agent** (`xd-polkit`, started with the
      session): an XP "Run As"-style password dialog (reason, which
      administrator when several qualify, password, Details) checked by
      polkit's own PAM helper; a wrong password can be retried. Tested
      with `pkcheck` and `xd-polkit --process PID`: the dialog, a wrong
      password, cancelling (reported as dismissed). **Untested: a correct
      password, and registering for a real XeonDesktop login session**
      (nested, the host desktop's agent owns the session, so ours exits).
      Restarted if it crashes (5 times in 30 s). Its error line ("The password is
      incorrect") isn't coloured.

## Linux settings

Applets for what Linux users set, beyond Windows'. All tested headless
against mocks or scratch directories; none yet against the real services.

- [~] Input methods: the compositor relays text-input-v3 to
      input-method-v2 (Fcitx 5 or IBus, started from Keyboard Properties'
      Text Services); tested with a stand-in IME and GTK3. Untested with a
      real Fcitx 5 or IBus.
- [~] Flatpak (`xd-flatpak`): per-app permissions as `flatpak override
      --user` files (checked against a real flatpak with a scratch app) and
      remotes. Untested: a system remote's change through flatpak's helper.
- [~] Automatic Updates (`xd-wuaucpl`): PackageKit, Flatpak and fwupd
      updates, the Host Security ID, the shell's scheduled check and XP's
      balloon. Untested against a real PackageKit transaction and a real
      fwupd (its flag bits and enums were written from memory).
- [~] System Configuration Utility (`xd-msconfig`): autostart entries and
      user services. Untested against a real user manager.
- [~] Dedicated graphics (switcheroo-control): "Run with dedicated graphics
      card" and per-program defaults. Untested on a real hybrid laptop.
- [~] Night light (compositor gamma ramp; GeoClue or typed location) and
      Color Management (`xd-color`, colord, vcgt calibration via lcms2).
      Untested: a real DRM gamma ramp (headless outputs have none), real
      colord and GeoClue. lcms2 reads the user's chosen ICC file inside the
      compositor; a malformed profile is a robustness risk to review.
- [~] Encrypted (LUKS) drives in My Computer and Disk Management: unlock,
      lock, change passphrase, remembered passphrases (gvfs's schema),
      crypttab at startup, encrypted formats. Untested on a real disk.
- [~] Passwords and Keys (`xd-keys`): Secret Service items, SSH keys and
      ssh-agent, GnuPG keys. Untested with a real gnome-keyring.
- [~] Privacy (`xd-privacy`): recent documents, the shell's old-file
      housekeeping, location, portal permissions. No GeoClue agent in the
      shell yet; untested with a real permission store and GeoClue.
- [~] Network drives and sharing in xd-files: Map Network Drive with
      reconnect at logon, My Network Places, Samba user shares. Untested
      with a real SMB/NFS server (needs gvfs-smb/gvfs-nfs).

## Testing

- [x] UI test suite (tests/ui/run.py, suite "ui": `meson test -C build
      --setup ui --suite ui`): each scenario in its own headless session
      (private mount namespace, scratch home, no system bus, sounds logged)
      checked through tools/xd-windows, files, settings and screenshot
      regions. Scenarios: Start menu and Run, Alt+Tab, maximize/minimize,
      Alt+F4, Aero Snap, Explorer's new/rename/delete, Display Properties'
      style, the desktop menu, Disk Management's Action menu (dbusmock),
      moving after a left-edge resize, and more since. Its own CI job
      (privileged container), passing on GitHub. The test sessions set
      XD_CLOCK_FIXED so the taskbar clock never ticks mid-scenario.
- [~] Under full parallel load, task_manager_users once timed out waiting
      for Logoff to end the guest's session (passes alone in 5s and in
      the next full run).
- [x] Fixed: GTK header-bar windows were drawn offset by their shadow
      margin (tiled or snapped ones fell short of the edges; UI test
      csd_window_tiled); Classic GTK buttons were 87x28 instead of
      Windows' 75x23; System Properties was 513 pixels wide (Windows':
      ~410).
- [x] Alt+letter opens that menu in our GTK apps (GTK on Wayland's synthetic
      enter during the menu bar's grab selected File).
- [x] Printer changes (Pause, Delete, Properties' Apply, Preferences) run
      off the main thread (xd_printers_run); Add Printer's name box has P.
      Printer Properties' Ports tab changes the device URI (Add Port from
      CUPS-Get-Devices, Configure Port) and a Sharing tab shares it, turning
      on the server's sharing when asked; tested against xd-cupsmock (a
      libcups stand-in: Arch's cupsd is root-only). Fixed: desktop submenus behind windows,
      long drop-downs, frame pacing, the Add Printer freeze, Network
      Connections closing under its dialogs.
- [x] CI (.github/workflows/ci.yml): GCC and Clang debug builds (ASan,
      UBSan, -Werror) with the unit tests, and a GCC -fanalyzer build.
