# TODO

Everything unfinished, untested or known to be wrong. Keep this current:
anything that ships incomplete gets a line here. "Untested" means the code
exists but nobody has exercised it with real input or on real hardware.

Legend: **[ ]** not done, **[~]** partly done, **[?]** done but untested.

## Platform and session

- [ ] **Real hardware**: the compositor has only run nested inside an X11
      session (wlroots X11 backend, software rendering). Never run on a TTY
      with DRM/KMS, libseat/logind, real GPUs or real input devices.
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
- [ ] **Session entry**: no `xeondesktop.desktop` for display managers, no
      session startup script, no `meson install` test.
- [~] **XWayland**: X11 apps run (Xwayland starts when the first one
      connects; DISPLAY is set for everything launched from the session).
      Tested with xterm: our frame, title and taskbar button, typing,
      maximize/restore, move, cascade placement, fullscreen, Alt+F4, its
      Ctrl+click menu (drawn over the taskbar), clean exit. Untested: X11
      dialogs/transients, Motif-undecorated windows, size-hint-fixed
      windows, apps that place themselves, HiDPI (Xwayland draws at 1x and
      is upscaled), clipboard and drag-and-drop between X11 and Wayland.
- [ ] **Keyboard layout** comes from `XKB_DEFAULT_*` only; no Keyboard applet.
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
- [~] **xdg-activation**: apps can bring their windows forward (tested:
      starting Task Manager again raises the open one). No focus-stealing
      prevention yet: any valid token raises the window.
- [~] **Lock Computer** (`xd-lock`, Win+L, the Windows 7 menu's Lock):
      Windows 2000's "Computer Locked" screen; Ctrl+Alt+Del opens "Unlock
      Computer", checked with PAM on a worker thread. If the locker dies
      while locked, the compositor starts it again (5 times a minute at
      most). Tested: locking, Ctrl+Alt+Del, a wrong password and its
      message, Caps Lock warning, restart after a crash, a keyboard
      unplugged and replugged. **Untested: unlocking with the right
      password** (please try it). The PAM service file
      (`/etc/pam.d/xeondesktop`, `auth include login`) is installed by
      `meson install`; until then `login` is used. Not yet: several
      monitors (the dialog shows on the first), Switch User, password
      change, the Windows Security dialog on Ctrl+Alt+Delete.
- [~] **Screen savers**: Blank Screen, Starfield Simulation, Mystify Your
      Mind, Beziers, Marquee Display and Flying Penguins, started by the
      shell after [ScreenSaver] Wait minutes idle (held off by video
      players), password protected or not; monitors off after MonitorOff
      minutes (wlr-output-power). Tested in a headless session (with
      XD_IDLE_SECONDS=1). Not yet: per-saver Settings (Marquee text,
      speeds), 3D Pipes / 3D Text, hardware tests of monitor power.
- [ ] Power actions (Shut down / Restart / Stand by / Hibernate) have only
      ever run **simulated** in nested sessions; never executed for real.

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
- [ ] Ctrl+Alt+Delete opens the Task Manager directly (as XP does with the
      Welcome screen). Windows 2000's Windows Security dialog (Lock, Log
      Off, Shut Down, Change Password, Task Manager) isn't done.
- [ ] Blur behind the Windows 7 Start menu's glass (wlroots' scene graph has
      no blur pass); until then the panel is opaque.
- [x] Resizing from the top/left edges keeps the opposite edge still.
- [x] Tested: Alt+Tab, Alt+F4 (Wayland and X11), Win+Up/Down (maximize,
      restore, minimize), Ctrl+Alt+T, Ctrl+Alt+Backspace, Ctrl+Esc,
      dragging borders and corners, the caption's minimize, maximize and
      close buttons, double-clicking the caption icon to close, and
      SIGTERM (Log Off) ending the session cleanly.
- Debug builds print LeakSanitizer reports from fontconfig/GTK caches at
  exit; only the unit tests use the suppression file.

## Shell (taskbar, Start menus, desktop)

- [ ] Tray / notification area (StatusNotifierItem), volume, network,
      battery; only the clock exists. No clock tooltip or calendar.
- [ ] Task buttons: no window icons, no grouping, no
      tooltips; overflow is clipped instead of wrapping or scrolling.
- [ ] Taskbar options: auto-hide, always on top, primary-only taskbar (Lock
      the Taskbar works: it hides the toolbar grips).
- [~] Right-click menus (XP): the taskbar's (Toolbars ▸ Quick Launch, Cascade,
      Tile Horizontally/Vertically and Undo, Show the Desktop, Task Manager,
      Lock the Taskbar, Properties) and a task button's window menu
      (Restore, Move, Size, Minimize, Maximize, Close; the default in bold),
      and the
      Start button's (Open, Explore, Properties, Open/Explore All Users).
      Move and Size run the compositor's keyboard move/size (arrows 8px, Ctrl
      1px, mouse too; Enter or a click keeps it, Esc undoes it). Tested:
      Move by arrows, Size by arrows, Esc restoring. Untested: mouse during
      keyboard move/size, Ctrl steps, corners (two edges). Not yet: the
      Address,
      Links, Desktop and New Toolbar toolbars, Search... in the Start
      button's menu, Quick Launch's menu and the clock's (Adjust
      Date/Time). Start button menu: Open tested; Explore, Properties and
      the All Users items untested. Tested: the taskbar menu, Tile Vertically, Undo Tile
      showing up, Minimize and the minimized window's menu. Untested:
      Cascade, Tile Horizontally, Undo itself, Lock toggling, Restore,
      Maximize, Close from the menu, several monitors.
- [ ] Quick Launch: tooltips, drag and drop to add and reorder.
- [ ] Desktop icons.
- [~] Windows XP Start menu. Done: layout in Luna, Watercolor and Classic,
      two-line Internet/E-mail entries, most used programs, cascading All
      Programs, Recent Documents and Control Panel submenus, Log Off and
      Turn Off Computer. Not yet: Set Program Access and Defaults, Printers
      and Faxes, Help and Support and Search (greyed out); pinning from the
      menu; keyboard navigation of the right column and footer; the
      highlight for newly installed programs; the large-icon/small-icon
      option. Untested: keyboard use, Log Off / Turn Off from the footer,
      Recent Documents and Control Panel submenus in the XP layout.
- [ ] Windows 7 menu: pinning/unpinning from the menu (edit
      `~/.config/xeondesktop/start-pinned` by hand), jump lists.
- [ ] Windows 2000 menu: Documents doesn't list recent files; a Programs
      folder taller than the screen is cut off instead of using columns.
- [ ] Greyed-out items: Search, Help, Devices and Printers, Default
      Programs, Switch user.
- [?] Start menu keyboard navigation after the focus fix (typing into the
      Windows 7 search box was tested; arrows/Enter/Esc in both menus not).
- [?] Windows 7 menu: Recent Items, the Shut Down ▸ submenu, Run, the
      frequently-used list showing launched apps.
- [?] Quick Launch buttons launching apps; Show Desktop's second click
      restoring windows.
- [?] Wallpaper modes other than Fill (Center, Tile, Stretch, Fit).

## Theme engine

- [ ] Classic colour schemes beyond Windows Standard, Windows Classic and
      High Contrast Black/White (Brick, Desert, Eggplant, Lilac, Maple,
      Marine, Plum, Pumpkin, Rainy Day, Rose, Slate, Spruce, Storm, Teal,
      Wheat, and Large/Extra Large variants).
- [ ] Verify scheme colour values against a reference: all but Windows
      Standard were written from memory.
- [~] Luna matched to a Windows XP screenshot (tools/luna-measure.py makes
      src/theme/luna_data.h): the active and inactive frame (caption,
      borders, corner cut-outs), caption buttons (faces within ~3% RMS),
      taskbar, Start button, task buttons, tray, push buttons and check
      boxes match at 100%. Text uses Wine's Tahoma when installed (see
      README); Trebuchet MS (captions) and Franklin Gothic (Start) have no
      free clone, so Tahoma Bold stands in. Not matched:
      the hover, pressed and focus states and the Restore and Help glyphs
      (not in the reference, so designed), radio buttons, tabs, scroll
      bars, group boxes, menus and the GTK theme's Luna section, which
      still uses the older approximations.
- [ ] Luna colour variants (Olive Green, Silver).
- [~] Whistler is the "Watercolor" look, matched against screenshots
      (frame, caption mosaic, caption buttons, buttons, fields, tabs, scroll
      bars, taskbar). Not yet: the Windows 2000 and 7 Start menus still use
      the older silvery Whistler colours; menus, group boxes, the window
      icon and inactive colours are guesses.
- [~] Fonts: Wine's Tahoma (tahoma.ttf, tahomabd.ttf) is used by our programs
      when installed; otherwise DejaVu/Noto stand in. Third-party GTK apps
      don't see it (it's registered per process, not system-wide).

## Dialogs and libxdui

- [ ] No input method (text-input-v3) support, no accessibility (AT-SPI),
      no radio buttons, no tooltips, no drag and drop in libxdui. libxdui
      windows can't be owned by another (Monitor Settings gets its own
      taskbar button). The slider (trackbar) is drawn Classic-style in
      every theme.
- [ ] Run: Browse... (`xd-choose-file`, the GTK chooser Display Properties
      uses, is ready for it).
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

- [ ] File operations: copy, move, delete (Recycle Bin), rename, new folder;
      clipboard; drag and drop.
- [~] Context menus: only Open / View / Refresh / Properties so far.
- [ ] Search, Favorites menu, rename in place.
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
- [ ] Details view: column resizing and reordering, more columns.
- [ ] Rubber-band selection; remembering the view per folder; View >
      Arrange Icons By; free space in the status bar.
- [ ] Special folders: My Computer (drives, mounts), Recycle Bin, Network
      Places, Control Panel (`control:`).
- [ ] The first arrow key in a fresh folder selects the second item.
- [?] Folders tree clicks, address bar typing, the Views menu, the
      thumbnail slider, Details header sorting, Show Hidden Files, opening
      files with the default app, live updates, very large folders.

## GTK3 theme

- [~] Generated GTK3 theme (`$XDG_DATA_HOME/themes/XeonDesktop`), rewritten
      by the compositor at start and on every appearance change. Our GTK
      apps reload it live. Covers buttons, entries, check/radio, menus,
      scroll bars, tabs, tree headers, panes, scales, tooltips, toolbars.
- [ ] Not yet styled: combo box pop-up lists, spin button arrows, info
      bars, switches, popovers (the Views menu), GtkFileChooser's sidebar
      and path bar, progress bar segments. Our GTK apps turn off dialog
      header bars (buttons at the bottom, our title bar); other apps'
      header-bar dialogs keep GTK's look.
- [ ] Other apps' GtkMessageDialogs show GTK's own title strip inside the
      window and stretched buttons; our apps use `xd_message_box` instead.
- [ ] The selected tab doesn't overlap its neighbours by 2px as in Windows
      (GTK ignores the negative margins).
- [?] Luna tabs, group boxes, combo boxes and check glyphs: only Classic
      and Whistler have been looked at since they were restyled.
- [?] Luna/Whistler visuals beyond the file manager's window, and every
      scheme other than Windows Standard: only checked to parse (unit test),
      not looked at.

- [ ] Third-party GTK3 apps pick up the theme at start (via `GTK_THEME`);
      they don't follow later changes until restarted.
- [ ] GTK4/libadwaita apps aren't themed (libadwaita ignores themes).

## Task Manager (xd-taskmgr)

- [ ] Applications' Status is always "Running": Windows shows "Not
      Responding" for hung windows; needs the compositor to report ping
      timeouts.
- [ ] Options > Always On Top (no Wayland protocol yet) and Hide When
      Minimized (needs the tray, which would also show the CPU meter).
- [ ] Windows menu: Tile Horizontally/Vertically and Cascade (the compositor
      can do it now through xd-window-arranger-v1, but only for the shell).
- [ ] Window icons only resolve for apps with an installed .desktop file
      matching their app ID; ours are installed by `meson install`.
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

- [ ] The `control:` folder and every applet other than Display, System
      and Taskbar and Start Menu (see docs/ARCHITECTURE.md for the list).
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
      through polkit and need an authentication agent, which the session
      doesn't start yet). Device Manager: no "by connection" view, no
      disable/uninstall/update driver. User Profiles: Delete, Change Type
      and Copy To are greyed (administrator jobs).
- [ ] A polkit authentication agent for the session (hostname changes,
      power profiles, and later printers, network, users).

## Testing

- [ ] Turn the xd-inject scenarios used so far into a repeatable UI test
      suite (start menu, Run, window management, file manager, settings).
      What works: a headless compositor (`WLR_BACKENDS=headless
      WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=N`, its own
      `XDG_CONFIG_HOME`), xd-inject for input (`hold`/`release` for
      modifiers), grim for screenshots. A nested session under X11 stops
      drawing while its window is covered, so screenshots of it go stale.
- [ ] CI: build with GCC and Clang, run unit tests, `-Danalyzer=true`.
