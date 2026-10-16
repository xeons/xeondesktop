# TODO

Everything unfinished, untested or known to be wrong. Keep this current:
anything that ships incomplete gets a line here. "Untested" means the code
exists but nobody has exercised it with real input or on real hardware.

Legend: **[ ]** not done, **[~]** partly done, **[?]** done but untested.

## Platform and session

- [ ] **Real hardware**: the compositor has only run nested inside an X11
      session (wlroots X11 backend, software rendering). Never run on a TTY
      with DRM/KMS, libseat/logind, real GPUs or real input devices.
- [ ] **Multiple monitors**: only ever one nested output. Hotplug, layout,
      per-output taskbars, maximize/work areas across outputs: untested.
- [?] **Automatic per-output scale** from EDID size: untested (only the
      `--scale` override has been used).
- [ ] **Session entry**: no `xeondesktop.desktop` for display managers, no
      session startup script, no `meson install` test.
- [ ] **XWayland**: not supported; X11 apps don't run in the session.
- [ ] **Keyboard layout** comes from `XKB_DEFAULT_*` only; no Keyboard applet.
- [ ] **Output management** (`wlr-output-management`): resolution, refresh,
      scale and arrangement can't be changed (Display Properties' Settings
      tab is read-only).
- [ ] **Idle detection and screen lock** (`ext-idle-notify`, session lock):
      needed for screen savers and "password protected".
- [ ] Power actions (Shut down / Restart / Stand by / Hibernate) have only
      ever run **simulated** in nested sessions; never executed for real.

## Compositor

- [ ] Window menu (Alt+Space, click on the caption icon, right-click caption).
- [~] Windows that ask to draw their own decorations (xdg-decoration
      client-side, or the KDE protocol's client mode, e.g. Firefox with tabs
      in the title bar) get no XP frame, plus an invisible 6px resize margin.
      Tested with Firefox: its own close button and edge resizing work.
      Firefox users can tick Customize Toolbar > Title Bar to get our frame.
- [ ] Clients that never negotiate but draw their own title bar anyway
      (GTK 4 / libadwaita apps) still get our frame too: a double title bar.
      Needs a per-app rule or an option to drop our frame.
- [ ] A setting to force our frame on every window (Firefox would then show
      its own buttons under ours).
- [ ] Client-decorated windows open at the top-left corner (no cascade
      offset), and have no window menu or keyboard move/size.
- [ ] Fullscreen requests are acknowledged but ignored.
- [ ] Modal/transient dialogs: no parent-relative placement or stacking.
- [ ] Win key and Ctrl+Esc don't open the Start menu (needs a
      compositor-to-shell channel).
- [ ] Ctrl+Alt+Delete opens the Task Manager directly. Windows 2000 first
      shows the Windows Security dialog (Lock, Log Off, Shut Down, Change
      Password, Task Manager); that needs a lock screen first.
- [ ] Blur behind the Windows 7 Start menu's glass (wlroots' scene graph has
      no blur pass); until then the panel is opaque.
- [ ] Resizing from the top/left edges moves the window before the client
      commits its new size, which can jitter.
- [?] Alt+Tab, Alt+F4, Win+Up/Down, Ctrl+Alt+T, Ctrl+Alt+Backspace: untested.
- [?] Resizing by dragging borders and corners: untested.
- [?] Caption close and minimize buttons: untested (maximize and
      double-click-to-unmaximize were tested).
- [?] Double-clicking the caption icon to close: untested.
- [?] Log off actually ending the session (SIGTERM path): untested.
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
      (Restore, Minimize, Maximize, Close; the default in bold), and the
      Start button's (Open, Explore, Properties, Open/Explore All Users).
      Not yet:
      Move and Size (keyboard move/size in the compositor), the Address,
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
      Programs, Switch user, Lock.
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
- [ ] Luna colour variants (Olive Green, Silver). Luna is an
      approximation; tune against references.
- [~] Whistler is the "Watercolor" look, matched against screenshots
      (frame, caption mosaic, caption buttons, buttons, fields, tabs, scroll
      bars, taskbar). Not yet: the Windows 2000 and 7 Start menus still use
      the older silvery Whistler colours; menus, group boxes, the window
      icon and inactive colours are guesses.
- [ ] Font fallback: Tahoma/Trebuchet aren't shipped; DejaVu/Noto stand in.

## Dialogs and libxdui

- [ ] No input method (text-input-v3) support, no accessibility (AT-SPI),
      no radio buttons, no tooltips, no drag and drop in libxdui.
- [ ] Run: Browse... (needs a file chooser).
- [ ] Display Properties:
  - Screen Saver tab (needs idle detection, saver plugins);
  - Settings tab (needs output management);
  - Effects: menu transitions, large icons, outline dragging, hiding
    keyboard cues;
  - Appearance: Advanced per-item editor; Background: Browse...
- [?] Display Properties' Apply/OK buttons (Apply was tested in Taskbar and
      Start Menu Properties, which shares the code path).
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
      bars, switches, popovers (the Views menu), GtkFileChooser,
      headerbar-based (CSD) dialogs, progress bar segments.
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

- [ ] The `control:` folder and every applet other than Display and
      Taskbar and Start Menu (see docs/ARCHITECTURE.md for the list).

## Testing

- [ ] Turn the xd-inject scenarios used so far into a repeatable UI test
      suite (start menu, Run, window management, file manager, settings).
- [ ] CI: build with GCC and Clang, run unit tests, `-Danalyzer=true`.
