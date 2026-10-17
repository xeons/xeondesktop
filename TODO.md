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
      in Display Properties. Untested: real monitors, hotplug, different
      scales side by side, the Start menu on the second monitor.
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
      Not yet: the Keyboard applet (Speed and Input Locales tabs), a layout
      indicator on the taskbar.
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
      `meson install`; until then `login` is used. Not yet: several
      monitors (the dialog shows on the first), Switch User. (Change
      Password is in Windows Security, below.)
- [~] **Screen savers**: Blank Screen, Starfield Simulation, Mystify Your
      Mind, Beziers, Marquee Display and Flying Penguins, started by the
      shell after [ScreenSaver] Wait minutes idle (held off by video
      players), password protected or not; monitors off after MonitorOff
      minutes (wlr-output-power). Tested in a headless session (with
      XD_IDLE_SECONDS=1). Not yet: per-saver Settings (Marquee text,
      speeds), 3D Pipes / 3D Text, hardware tests of monitor power.
- [ ] Power actions (Shut down / Restart / Stand by / Hibernate) have only
      ever run **simulated** in nested sessions; never executed for real.

## Packaging

- [~] GitHub Actions builds a `.deb` (Debian testing and unstable, the only
      releases with wlroots 0.20) and an Arch Linux / CachyOS package on
      every push and pull request, and attaches them to a release for v*
      tags. The Debian dependencies come from dpkg-shlibdeps; the Arch ones
      were checked with namcap. Built, tested and installed locally in
      podman containers (debian:testing, archlinux, cachyos/cachyos). The
      workflow passes on GitHub; the release job is untested until the
      first v* tag. Not yet: Ubuntu
      (no wlroots 0.20 in a release), arm64 builds, man pages (lintian's
      only complaint), an apt/pacman repository to install from.

## Compositor

- [~] Window menu from the window itself: Alt+Space, a click on the caption
      icon, a right-click on the title bar and xdg_toplevel.show_window_menu
      all open the shell's window menu (via xd-window-arranger-v1). Tested:
      Alt+Space, icon click, right-click, double-clicking the icon to close.
      Untested: show_window_menu from a client, keyboard use of the menu
      when opened by Alt+Space, taskbars not at the bottom (the menu is
      positioned relative to a bottom taskbar).
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
      the blur buffers on VirtualBox 3D (headless). **Untested: how the
      blur looks on a real GPU.** Not yet: blur behind the Windows 7 Start
      menu's glass (its panel is opaque), colourisation.
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
- [~] Windows 7's taskbar (Aero): pinned and running programs share a row
      of icon buttons (taskbar-pinned; the browser, Files and a media
      player by default), running ones framed with stacked edges for more
      windows; click activates or cycles, middle-click starts another,
      dragging reorders pins. Jump lists: Recent (recently-used.xbel, per
      program), Tasks (desktop actions), pin/unpin, close. Taskbar
      Properties' "Taskbar buttons" choice. Tested headless. Not yet:
      thumbnails, pinning inside a jump list, pinning by dropping a file,
      Windows 7's own jump list look; sizes are from memory; "Combine when
      taskbar is full" combines every program at once.
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

## Shell (taskbar, Start menus, desktop)

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
      Not yet: 12-hour time entry, styled spin buttons.
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
      a real battery, real Bluetooth devices. Not yet: a status dialog for
      the connection, pairing new Bluetooth devices, enterprise (802.1X)
      networks in our own UI (nm-connection-editor opens instead), the
      volume flyout's keyboard use, XP's low-battery warnings.
- [~] Task buttons: the application's icon (desktop file found from the
      app id or WM_CLASS); tooltips with the whole title when it's cut
      short; pages with up/down arrows (and the wheel) when buttons would
      get narrower than 56px; Group similar taskbar buttons ("3 Firefox",
      a menu of its windows, Minimize Group / Close Group) once they'd get
      narrower than 100px; the group's right-click has XP's Cascade, Tile
      Horizontally/Vertically (xd-window-arranger-v1 v3's arrange_app),
      Minimize Group and Close Group. A drag held over a task button
      brings its window forward. Tested in Luna with 30 and 14+2 windows.
      Not yet: the window frame's caption icon (still generic).
- [~] Taskbar options in Taskbar and Start Menu Properties: Auto-hide
      (tucked to 2px, out on hover or a menu), Keep on top (off: the bottom
      layer), Show the clock, Show the taskbar on all displays (off: only
      the first monitor's); Lock the Taskbar hides the toolbar grips.
      Tested headless, two outputs for all displays. Untested: Auto-hide
      with the Windows key, real monitors. With all displays off, Alt+Space
      can't open the window menu on monitors without a taskbar.
- [~] Taskbar on any edge and in several rows ([Taskbar] Edge, Rows,
      SideWidth): rows of task buttons and tray icons on top and bottom
      taskbars, a stacked layout on side ones; menus, paging, grouping,
      Auto-hide and drops follow the edge. Unlocked, it's dragged to
      another edge and resized by its inner edge, as in XP. Tested
      headless in Luna and Classic: all four edges, 1-3 rows, moving and
      resizing by mouse, the Start menus. Untested: Watercolor, real
      monitors, the window menu (Alt+Space) from a side or top taskbar,
      several monitors. Not yet: XP's Show Quick Launch toolbar text, the
      clock's date lines on a tall taskbar.
- [~] Right-click menus (XP): the taskbar's (Toolbars ▸ Quick Launch, Cascade,
      Tile Horizontally/Vertically and Undo, Show the Desktop, Task Manager,
      Lock the Taskbar, Properties) and a task button's window menu
      (Restore, Move, Size, Minimize, Maximize, Close; the default in bold),
      and the
      Start button's (Open, Explore, Properties, Open/Explore All Users).
      Move and Size run the compositor's keyboard move/size (arrows 8px, Ctrl
      1px, mouse too; Enter or a click keeps it, Esc undoes it). Tested:
      Move by arrows, Size by arrows, Esc restoring. Untested: mouse during
      keyboard move/size, Ctrl steps, corners (two edges). The clock's menu
      adds Adjust Date/Time; Quick Launch's has its own (below). Not yet:
      the Address, Links, Desktop and New Toolbar toolbars, Search... in
      the Start button's menu. Start button menu: Open tested; Explore,
      Properties and
      the All Users items untested. Tested: the taskbar menu, Tile Vertically, Undo Tile
      showing up, Minimize and the minimized window's menu. Untested:
      Cascade, Tile Horizontally, Undo itself, Lock toggling, Restore,
      Maximize, Close from the menu, several monitors.
- [~] Quick Launch: tooltips (the shortcut's name); a shortcut's menu
      (Open, Delete, Properties) and the toolbar's (Open Folder, Close
      Toolbar); files and applications dropped on it become shortcuts
      where they land (a mark shows where), and its buttons can be dragged
      along to rearrange them. xd-files drags its selection out (to Quick
      Launch or any program). Tested headless with a GTK drag source and
      xd-files. Not yet: dragging from our own Start menu, large icons,
      Show Text / Show Title.
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
      column and the footer. Not yet: pinning from the menu; letter
      shortcuts; the
      highlight for newly installed programs; the large-icon/small-icon
      option. Untested: keyboard use, Log Off / Turn Off from the footer,
      Recent Documents and Control Panel submenus in the XP layout.
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
- [ ] Switch User locks first, and xd-lock doesn't answer logind's Unlock,
      so returning asks for the password twice. XP's Log Off dialog has no
      Switch User button yet.
- [?] Windows 7 menu: Recent Items, the Shut Down ▸ submenu, Run, the
      frequently-used list showing launched apps.
- [?] Quick Launch buttons launching apps; Show Desktop's second click
      restoring windows.
- [?] Wallpaper modes other than Fill (Center, Tile, Stretch, Fit).

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
- [~] Luna colour variants Olive Green and Silver (LunaColor=HomeStead /
      Metallic): Blue's measured artwork recoloured to XP's documented
      colours, live, GTK theme included. Approximate shading (no reference
      screenshots); push buttons, check boxes, edit borders and the
      inactive Close stay Blue's.
- [~] Aero (Windows 7), style_aero.c: translucent sky-blue glass frames
      with a sheen and faint streaks, rounded corners, the dark outline
      and light inner edge, the grouped caption strip with a red Close,
      the title (Segoe UI, Open Sans standing in) on a soft white glow;
      a glass taskbar with a glossy round Start orb and glass task
      buttons; Windows 7's light buttons, check boxes, menus and dialog
      colours. Not yet: blur behind the glass (needs the GPU, see the
      compositor), Windows 7's taskbar (icon-only, pinned), Aero's GTK
      theme beyond the shared rules, colourisation choices.
- [~] Whistler is the "Watercolor" look, matched against screenshots
      (frame, caption mosaic, caption buttons, buttons, fields, tabs, scroll
      bars, taskbar). Not yet: the Windows 2000 and 7 Start menus still use
      the older silvery Whistler colours; menus, group boxes, the window
      icon and inactive colours are guesses.
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

## Dialogs and libxdui

- [ ] No input method (text-input-v3) support, no accessibility (AT-SPI),
      no radio buttons, no tooltips, no drag and drop in libxdui. libxdui
      windows can't be owned by another (Monitor Settings gets its own
      taskbar button). The slider (trackbar) is drawn Classic-style in
      every theme.
- [x] Run: Browse... opens the GTK chooser (xd-choose-file) with XP's
      Programs filter and puts the choice in the Open box.
- [~] Display Properties:
  - Screen Saver tab: saver list with a live preview, Preview, Password
    protected, Wait, and "Turn off monitor" (instead of Power Options).
    Tested. The Settings button stays greyed (savers have no options yet).
  - Settings tab: monitor arrangement by dragging (snaps edge to edge),
    resolution slider, refresh rate, scale, orientation, extending onto a
    monitor, and Windows' 15-second "keep these settings?" check that
    reverts by itself. Tested with two headless outputs: scale change,
    revert on timeout, dragging a monitor underneath and keeping it. Not
    tested with real modes or rotation. No Identify, no Advanced.
  - Background: Browse... (GTK file chooser). Tested.
  - Effects: menu transitions, large icons, outline dragging, hiding
    keyboard cues are still greyed out;
  - Appearance: Advanced per-item editor.
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
- [ ] Favorites menu; the toolbar's Search button is still greyed (it
      could open xd-search). My Network Places is titled network:///.
- [~] Properties dialog. Tested: General for a file, Permissions (ticking
      Group Write and Apply changed 644 to 664), Details, and the size and
      contents count for a folder plus a file (matches find/du exactly).
- [?] Properties, untested: renaming and Hidden (renames to/from a dot
      name), Read-only, Advanced (chattr) apply, Change... (default app),
      changing the group, "Apply changes to all files and folders inside",
      tri-state check boxes with several mixed items, the error paths.
- [ ] Properties: no Shortcut tab for .desktop files, no per-type tabs
      (image size, audio tags), no ACLs or SELinux context.
- [ ] `xd-thumbnailer` (sandboxed): only thumbnails already in the
      freedesktop cache are shown.
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
- [ ] My Computer: the drive Properties' pie chart; groups in the List
      and Details views. Network Places and Control Panel (`control:`).
- [x] The first arrow key in a fresh folder selected the second item.
- [?] Folders tree clicks, address bar typing, the Views menu, the
      thumbnail slider, Details header sorting, Show Hidden Files, opening
      files with the default app, live updates, very large folders.

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
- [ ] Not yet styled: GtkFileChooser's sidebar and path bar; combo
      pop-ups are menus (Windows drops down a list box). Our GTK apps turn off dialog
      header bars (buttons at the bottom, our title bar); other apps'
      header-bar dialogs keep GTK's look.
- [ ] Other apps' GtkMessageDialogs show GTK's own title strip inside the
      window and stretched buttons; our apps use `xd_message_box` instead.
- [ ] The selected tab doesn't overlap its neighbours by 2px as in Windows
      (GTK ignores the negative margins).
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

- [ ] Third-party GTK3 apps pick up the theme at start (via `GTK_THEME`);
      they don't follow later changes until restarted.
- [ ] GTK4/libadwaita apps aren't themed (libadwaita ignores themes).

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
      a frozen program. Not yet: XP's ghost window, and the box for a
      window that only stops responding after the close request.
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
- [ ] Peak Mem Usage only covers the time Task Manager has been open.
- [ ] Not Windows: no Users or Networking tab (those are XP).
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
      on resume; lid/power buttons read-only from logind), Regional and
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
- [ ] Applets: not done: UPS; Power Options' Alarms and Power Meter tabs;
      on-screen status indicators for the accessibility features, the
      Sound tab (SoundSentry/ShowSounds); Customize Regional Options' fields
      glibc can't express (list separator, two-digit years, long date...);
      limited users see administrator tasks in User Accounts; other GTK
      apps don't follow the double-click time or caret blink.
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
      format, resize, delete), real polkit prompts for services, packages
      and printers, real pairing, a real controller's test page, real
      printers/scanners/cameras, DNS-SD printer discovery, a real systemd
      user manager running scheduled tasks.
- [~] Session: a real login hands the user manager WAYLAND_DISPLAY,
      DISPLAY, XDG_* and the session bus, and starts
      xeondesktop-session.target (BindsTo graphical-session.target), which
      xeondesktop-session stops when the compositor exits; so "When I log
      on" tasks run and scheduled programs can open windows. Tested against
      a mock user manager; untested with a real login.
- [ ] Add or Remove Programs: "Last Used On" only for programs that open
      documents (the Start menu's launch counts have no dates).
- [~] **System Properties** (`xd-sysdm`, Windows 2000's layout, Linux
      inside): General (distribution, kernel, owner, machine ID, CPU,
      firmware, RAM); Network Identification (host name and description
      through systemd-hostnamed, domain or Samba workgroup, computer type);
      Hardware (Kernel Modules, Driver Signing = Secure Boot / module
      signature enforcement / lockdown / taint, Device Manager from udev
      with General/Driver/Details per device, Power Profiles through
      power-profiles-daemon); User Profiles (home folders with their size);
      Advanced (Performance = scheduler and swap/zram, Environment
      Variables in `~/.config/environment.d`, Startup and Recovery = the
      boot loader's entries from EFI variables, panic and core dump
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
      Not restarted if it crashes. Its error line ("The password is
      incorrect") isn't coloured.

## Testing

- [x] UI test suite (tests/ui/run.py, suite "ui": `meson test -C build
      --setup ui --suite ui`): each scenario in its own headless session
      (private mount namespace, scratch home, no system bus, sounds logged)
      checked through tools/xd-windows, files, settings and screenshot
      regions. Scenarios: Start menu and Run, Alt+Tab, maximize/minimize,
      Alt+F4, Aero Snap, Explorer's new/rename/delete, Display Properties'
      style, the desktop menu, Disk Management's Action menu (dbusmock),
      moving after a left-edge resize. Its own CI job (privileged
      container); not yet seen running on GitHub.
- [ ] Alt+letter opens the first menu in our GTK apps (Alt+A in Disk
      Management opens File); F10 then the letter works.
- [ ] Bugs found in passing: a submenu of the desktop's context menu can
      draw behind a window; libxdui's combo drop-down shows 8 items and no
      scroll bar; the Add Printer wizard adds the printer on the main
      thread (freezes for seconds; its Cancel stays live), and other
      printer dialogs block on an unreachable CUPS server; closing Network
      Connections' main window quits under open dialogs; libxdui draws
      without frame callbacks (a slider drag can outrun the display).
- [x] CI (.github/workflows/ci.yml): GCC and Clang debug builds (ASan,
      UBSan, -Werror) with the unit tests, and a GCC -fanalyzer build.
