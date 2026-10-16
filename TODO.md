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
- [ ] Fullscreen requests are acknowledged but ignored.
- [ ] Modal/transient dialogs: no parent-relative placement or stacking.
- [ ] Win key and Ctrl+Esc don't open the Start menu (needs a
      compositor-to-shell channel).
- [ ] Blur behind the Windows 7 Start menu's see-through panel (wlroots'
      scene graph has no blur pass).
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
- [ ] Task buttons: no window icons, no right-click menu, no grouping, no
      tooltips; overflow is clipped instead of wrapping or scrolling.
- [ ] Taskbar options: auto-hide, lock, always on top, primary-only taskbar.
- [ ] Quick Launch: tooltips, drag and drop to add and reorder.
- [ ] Desktop icons.
- [ ] Windows XP Start menu layout (2000 and 7 exist).
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
- [ ] Luna colour variants (Olive Green, Silver). Luna and Whistler are
      approximations; tune against references.
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
- [ ] Context menus, Properties dialog, Search, Favorites menu.
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
- [ ] Not yet styled: combo boxes and their pop-ups, spin button arrows,
      info bars, switches, popovers (the Views menu), GtkFileChooser,
      headerbar-based (CSD) dialogs, progress bar segments.
- [ ] Check/radio marks use symbolic icons rather than Windows' tick and
      dot glyphs; Luna's orange hover rim on check boxes is approximated.
- [?] Luna/Whistler visuals beyond the file manager's window, and every
      scheme other than Windows Standard: only checked to parse (unit test),
      not looked at.
- [ ] Clicking GTK's menu bar with injected input (xd-inject) doesn't open
      the menu; Alt+letter does. Investigate (pointer enter serial?).
- [ ] Third-party GTK3 apps pick up the theme at start (via `GTK_THEME`);
      they don't follow later changes until restarted.
- [ ] GTK4/libadwaita apps aren't themed (libadwaita ignores themes).

## Control Panel

- [ ] The `control:` folder and every applet other than Display and
      Taskbar and Start Menu (see docs/ARCHITECTURE.md for the list).

## Testing

- [ ] Turn the xd-inject scenarios used so far into a repeatable UI test
      suite (start menu, Run, window management, file manager, settings).
- [ ] CI: build with GCC and Clang, run unit tests, `-Danalyzer=true`.
