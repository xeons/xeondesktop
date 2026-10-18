# XeonDesktop architecture

A Windows 2000, XP and 7-style desktop environment for Linux (Wayland),
written in strict C23. This describes how it is built today; TODO.md tracks
what's left.

## Goals

- A faithful Windows 2000 / XP / 7 desktop: taskbar, Start menu, Quick
  Launch, notification area, desktop icons, Explorer, Control Panel, Task
  Manager.
- Three Start menu layouts: **Windows 2000** (cascading, with the side
  banner), **Windows XP** (two columns, pinned and most used) and **Windows
  7** (one column with search and jump lists).
- Visual styles as in Windows: **Classic** (2000, with XP's 22 colour
  schemes), **Luna** (XP: Blue, Olive Green, Silver and Royale's Energy
  Blue), **Whistler** (the pre-release Watercolor look) and **Aero**
  (Windows 7, with blurred glass on a GPU).
- Per-monitor DPI and full multi-monitor support.
- Memory safety as far as C allows (see below).

## Processes

Every component is its own process. In Windows, explorer.exe is both the
taskbar and the file manager, so a slow network folder or a bad thumbnail
can freeze the taskbar; here a crash or hang only takes down the program it
happens in, and the compositor restarts the shell and the lock screen if
they die.

| Program | Toolkit | Role |
|---|---|---|
| `xd-compositor` | wlroots 0.20 | Wayland compositor and window manager: server-side frames drawn by the theme engine, focus, Alt+Tab, Aero Snap, per-output scale, layer-shell work areas, Xwayland, keyboard shortcuts, the accessibility keys, "Not Responding" detection, title bar icons, window sounds. |
| `xd-compositor-fx` | wlroots + SceneFX | The same compositor on SceneFX's scene graph: Aero's blurred glass and window shadows. `xd-compositor` hands over to it when a GPU renderer is available. |
| `xd-shell` | libxdui | Taskbar (or Windows 7's icon taskbar), Start menus, Quick Launch, notification area (StatusNotifierItem host with dbusmenu; built-in clock, volume, network, Bluetooth and battery icons), desktop (wallpaper and icons), screen saver timer. A layer-shell client. |
| `xd-lock` | libxdui | Lock Computer (ext-session-lock) and the screen savers. |
| `xd-files` | GTK3 | Explorer: folders, My Computer, Recycle Bin, Control Panel (`control:`), file operations, Properties. |
| `xd-taskmgr` | GTK3 | Task Manager, with its tray CPU meter. |
| `xd-desk`, `xd-taskbar` | libxdui | Display Properties and Taskbar and Start Menu Properties, whose previews must match the compositor's frames pixel for pixel. |
| `xd-run`, `xd-shutdown`, `xd-endtask`, `xd-message` | libxdui | Run, Shut Down / Log Off, End Program, message boxes for programs without dialogs of their own. |
| `xd-security` | GTK3 | Windows Security (Ctrl+Alt+Delete) and Change Password. |
| `xd-polkit` | GTK3 | The polkit authentication agent. |
| `xd-search`, `xd-help`, `xd-defaults` | GTK3 | Search for Files or Folders, Help and Support, Set Program Access and Defaults. |
| `xd-control` | none | Opens `control:` in `xd-files`, or one applet (`xd-control desk`). |
| `xd-access`, `xd-mouse`, `xd-keyboard`, `xd-sound`, `xd-power`, `xd-timedate`, `xd-intl`, `xd-folders`, `xd-fonts`, `xd-users`, `xd-sysdm`, `xd-netconn`, `xd-bluetooth`, `xd-joy`, `xd-printers`, `xd-scanners`, `xd-appwiz`, `xd-schedtasks`, `xd-eventvwr`, ... | GTK3 | Control Panel applets and Administrative Tools, one program each. |

Development tools: `xd-themeview` (renders theme previews to PNG),
`xd-inject` (virtual pointer and keyboard for UI tests), `xd-windows`
(lists windows for UI tests), `tools/luna-measure.py` (measures Luna
artwork from reference screenshots into colour tables).

### Libraries (static, linked into the programs)

| Library | Contents |
|---|---|
| `libxdcore` | Checked allocation, arenas, length-carrying strings, lists. |
| `libxdtheme` | Colour schemes, metrics, the four visual styles, DPI-aware drawing, frame layout and hit testing, the GTK CSS generator. |
| `libxdsettings` | `desktop.ini` and the other settings files, watching them, the generated GTK theme and Qt palette, installed icon/GTK/Qt themes, the sound scheme and its player. |
| `libxdui` | Our own Win32-style widget toolkit and Wayland client plumbing: shm buffers, fractional scaling, popups, text editing, icons. |
| `libxdappicons` | Icon theme lookup (GTK3's GtkIconTheme, GTK not initialised) and which application a window belongs to (GDesktopAppInfo). Shared by libxdui and the compositor. |
| `libxdfolder` | The folder view widget, places (My Computer, Recycle Bin), file operations: used by `xd-files` and the desktop. |
| `libxdgtkapp` | Shared GTK3 helpers: theme and settings following, message boxes, file chooser. |
| `libxdsaver` | The screen savers, shared by `xd-lock` and Display Properties' preview. |
| `libxdrename` | Rename helpers (GIO only), shared by the shell and `xd-files`. |

### Toolkit decision: hybrid

Decided 2026-10-15. Programs where the exact look matters most (the shell,
the lock screen, Display Properties and the small system dialogs) use
**libxdui**, drawn with the same theme engine as the window frames. Larger
programs (Explorer, Task Manager and most Control Panel applets) use
**GTK3**, which brings input methods, accessibility (AT-SPI), drag and drop
and mature list and tree views that would take long to rebuild safely in C.

To make GTK3 programs match, the compositor **generates a GTK3 theme from
the active style and colour scheme** (`~/.local/share/themes/XeonDesktop`)
whenever the appearance changes; our GTK apps reload it at once, others get
it through `GTK_THEME`. The same generator writes a Qt palette
(`QT_GUI_GTK_JSON`) so that Qt 5 and 6 programs, which the session points
at Qt's own GTK platform theme (`QT_QPA_PLATFORMTHEME=gtk3`), follow too,
with Qt's Windows style for Classic and Fusion otherwise. The user can pick
any installed GTK theme, Qt style and icon theme instead in Display
Properties; variables the user sets themselves are left alone. GTK4 and
libadwaita are avoided because libadwaita ignores themes.

The folder view is **our own GTK3 widget** rather than `GtkIconView`, which
slows down badly on folders with thousands of files (Thunar ships its own
fork for this reason). It draws all five views with cairo, lays out only
what's visible, and keeps items on screen while a folder refreshes.

## Wayland protocols

The shell and the other programs are ordinary clients. Besides the core
and xdg-shell, they use:

- `wlr-layer-shell` (taskbar, desktop, menus, savers), with exclusive zones
  for the work area;
- `wlr-foreign-toplevel-management` (task buttons, Task Manager's
  Applications, Alt+Tab-independent window lists);
- `ext-session-lock-v1` (Lock Computer), `ext-idle-notify-v1` (screen saver,
  monitor power-off), `wlr-output-power-management`;
- `wlr-output-management` (Display Properties' Settings tab, with Windows'
  15-second revert);
- `wp_fractional_scale_v1` and `wp_viewporter`, `wp_cursor_shape_v1`,
  `xdg-activation-v1`;
- `xdg-decoration` and KDE's server decoration (GTK 3 uses it) to put our
  frames on every window;
- `wlr-data-control` and `ext-data-control` (clipboard managers and
  VirtualBox's shared clipboard);
- `xd-window-arranger-v1`, our private protocol, offered only to the shell
  the compositor started: Cascade/Tile Windows with Undo, Aero Peek (of
  the desktop or one window), which window each
  `ext-foreign-toplevel-list` handle is, and the lid switch and the power
  and sleep keys (for Power Options' Power buttons, which the shell
  carries out holding logind's block inhibitors; with
  `--allow-virtual-input` every client sees it, for `xd-inject lid`);
- `ext-image-copy-capture-v1` with `ext-foreign-toplevel-image-capture-source-v1`
  and `ext-foreign-toplevel-list-v1`, also offered to the shell alone:
  Windows 7's taskbar thumbnails, captured into shared memory at thumbnail
  size;
- `wlr-virtual-pointer` and `virtual-keyboard`, only with
  `--allow-virtual-input` (UI tests).

## Compositor

- **Rendering.** wlroots' scene graph. On a GPU, `xd-compositor` hands over
  to `xd-compositor-fx` (SceneFX: blur, shadows, rounded corners for Aero).
  On a virtual GPU (vmwgfx, virtio-gpu, qxl, VirtualBox), whose GL is too
  slow, it renders with pixman and draws the cursor in software.
- **Frames** are drawn by libxdtheme into a buffer per window, redrawn only
  when size, scale, focus, hover, title or icon change. Title bar icons come
  from each window's application (its desktop file's Icon, else an icon
  named after the app id, else the generic program), loaded in a thread so
  theme lookups and image decoding never stall the compositor.
- **Window management**: cascade placement, maximize/minimize/restore,
  Aero Snap (edges and Win+arrows, across monitors), resizing from the
  top/left keeping the opposite edge still, the window menu with keyboard
  Move and Size, Alt+Tab, Show Desktop.
- **Not Responding**: windows that stop answering pings get "(Not
  Responding)" and End Program; a program whose main loop runs but ignores
  input (a stuck grab) can't be seen this way.
- **Input**: libinput settings from Mouse Properties, XKB layouts and
  repeat from Keyboard, and the accessibility features (StickyKeys,
  FilterKeys/SlowKeys/BounceKeys, ToggleKeys, MouseKeys, ClickLock) with
  XP's activation shortcuts.
- **Session.** In a real login the compositor gives the systemd user
  manager the display (`WAYLAND_DISPLAY`, `DISPLAY`, `XDG_*`, the theme
  variables) and starts `xeondesktop-session.target`, which binds
  `graphical-session.target` and pulls in `xdg-desktop-autostart.target`,
  so autostart entries and "When I log on" scheduled tasks run.
  `xeondesktop-session` stops it when the compositor exits. Nested and test
  sessions never touch the user manager. A drop-in makes VirtualBox's
  guest client share the clipboard as a Wayland client.

## Display and DPI model

- All theme metrics are in **logical units** (1 unit = 1 px at 96 DPI).
- Each output has its own scale (1.0, 1.25, 1.5, 2.0, ...). Surfaces are
  rendered at the scale of the output they're on.
- `xd_px()` converts logical units to whole device pixels, never rounding a
  visible line down to 0, which keeps 1-unit bevels crisp at fractional
  scales.
- Each output gets its own taskbar, or only the primary one. Windows
  maximize to their own output's work area. A frame across two outputs
  takes the scale of the output holding most of it.

## Theme engine

- **Colour schemes** (`data/schemes/*.ini`) use the Windows registry colour
  names (`ActiveTitle`, `ButtonFace`, ...) plus `[Metrics]` and `[Fonts]`;
  a scheme lists only what differs from Windows Standard. All 22 of XP's
  classic schemes ship, with every colour taken from a published table of
  XP's system colours and cross-checked against ReactOS (cited in each
  file).
- **Classic** draws everything from the scheme.
- **Luna** draws from colour tables measured from screenshots of real
  Windows XP (`tools/luna-measure.py` writes `luna_data*.h`): Blue, Silver,
  Olive Green and Energy Blue each have their own caption, frame, caption
  buttons, taskbar, Start button, task buttons and tray, and the XP Start
  menu takes the variant's colours. What no reference shows (hover and
  pressed states, some inactive windows) is derived.
- **Whistler** and **Aero** have their own renderers; Aero's glass is
  translucent and the compositor blurs what's behind it.
- All artwork is drawn procedurally; no bitmaps from Microsoft products are
  shipped. Free fonts stand in for Tahoma, Trebuchet MS and Franklin Gothic,
  and the real ones are used if installed.

## Settings

`~/.config/xeondesktop/desktop.ini` is the single source of truth for the
desktop's own settings, with separate files for monitors, pins, folder
views and other state. Programs write it atomically (write, then rename)
and merge only the keys they changed, so two dialogs can't undo each
other's changes. The compositor, the shell and our GTK apps watch it with
inotify and apply changes live. A watched file needs no daemon, keeps
working if any one component crashes, and is easy to inspect or edit.
Unknown keys are ignored so older builds can read newer files; values are
validated (a scheme is a file stem, never a path).

System settings (time zone, locale, accounts, network, printers, disks,
services, packages) go through the system's own services and their polkit
checks, never through programs running as root.

Settings shared with other desktops are written only where Linux expects
them, and only when the user changes them: the GTK theme and icon theme in
GTK's `settings.ini` and GNOME's interface GSettings (the icon theme in use
is also written at login, since GTK on Wayland reads it only there), MIME
defaults in `mimeapps.list`, environment variables in
`~/.config/environment.d`, customised formats as a compiled locale.

## File manager (`xd-files`)

Modelled on Windows XP's Explorer windows:

- **Views**: Thumbnails, Tiles, Icons, List and Details (sortable,
  resizable and reorderable columns chosen from the header), with a
  thumbnail size slider, Show in Groups, Auto Arrange and free placement,
  remembered per folder.
- Folders tree, address bar, Back/Forward/Up, rename in place, drag and
  drop, cut/copy/paste through the shell's clipboard, Send To, multi-level
  Undo, Properties dialogs.
- Special folders: My Documents, My Computer (every mount from GIO's
  volume monitor, with mount and eject), Recycle Bin (freedesktop trash,
  including other drives' `.Trash-UID`), My Network Places (gvfs), Control
  Panel (`control:`).
- **File operations use GIO**: copy/move with progress and conflict
  dialogs, trash, mounting through UDisks2, MIME detection, change
  monitoring, SMB/SFTP through gvfs. A running job keeps the process alive
  until it finishes, even if its window is closed.
- **Drives talk to UDisks2 directly** (`udisks.c`): a drive's name is its
  file system's label, renamed with `Filesystem.SetLabel`; the Tools tab's
  Check Now... calls `Check` (read-only) or `Repair`; the Hardware tab lists
  its `Drive` objects. polkit asks, as for Disk Management. Disk Cleanup
  (`cleanup.c`, no GTK) deletes only the user's own disposable files (the
  Recycle Bin, the thumbnail cache, known browser caches, week-old entries
  in the temporary folders), walking with `openat()` and never following
  links or crossing into another file system.
- **Thumbnails** follow the freedesktop thumbnail spec, so they're shared
  with other programs. Those missing from the cache are made by
  `xd-thumbnailer`, a process per file, a few at a time, what's on screen
  first; a folder change stops them. Decoding untrusted files never runs
  inside the file manager or the shell: `xd-thumbnailer` decodes (with
  gdk-pixbuf, or an installed `.thumbnailer` such as ffmpegthumbnailer) in
  a bubblewrap sandbox with no network and nothing of the file system but
  /usr and the one file, read-only, under a seccomp filter and limits on
  memory, CPU time and file size. Only raw pixels come out; the PNG is
  written outside. glycin's own nested sandbox is refused there, so its
  loader runs directly inside ours.

## Control Panel

As in Windows, where Control Panel was a special folder and each applet a
separate `.cpl` you could run on its own:

- **Each applet is its own program.** A crash in one doesn't affect the
  others, and each opens directly (`xd-control desk`, as `control
  desk.cpl`).
- **Control Panel is the virtual `control:` folder in `xd-files`**, with
  XP's Category View and Classic View. Applets register with a `.desktop`
  file (`Categories=...;X-XeonDesktop-ControlPanel;`,
  `X-XeonDesktop-ControlPanel-Category=` for XP's categories,
  `NoDisplay=true` to stay out of All Programs), so a new one just appears.
- **Applets never run as root**; nothing is setuid. Long system calls
  (polkit prompts, UDisks, CUPS, PackageKit) are made asynchronously so a
  password prompt never freezes a window.

| Applet | Backend |
|---|---|
| Accessibility Options | the compositor (StickyKeys, FilterKeys, ToggleKeys, MouseKeys, High Contrast) |
| Add or Remove Programs | PackageKit and Flatpak |
| Administrative Tools | Services (systemd), Event Viewer (journald), Disk Management (UDisks2: partitioning, formatting, resizing), Device Manager (udev) |
| Bluetooth Devices | BlueZ |
| Date and Time | timedated; timesyncd (org.freedesktop.timesync1) and systemd for the time server and Update Now |
| Display | `desktop.ini`, `monitors.ini`, `wlr-output-management` |
| Flatpak | libflatpak (remotes), the user's override files (`flatpak override --user`'s format) |
| Folder Options | the file manager's settings, `mimeapps.list`, the user's shared-mime-info database |
| Fonts | fontconfig; printing through GtkPrintOperation |
| Game Controllers | udev and libevdev |
| Keyboard, Mouse | the compositor (XKB, libinput), XCursor themes |
| Network Connections | NetworkManager (libnm) |
| Power Options | logind, UPower, the shell's idle timers |
| Printers and Faxes | CUPS |
| Regional and Language Options | localed; customised formats compiled with localedef |
| Scanners and Cameras | SANE, libgphoto2 |
| Scheduled Tasks | systemd user timers |
| Sounds and Audio Devices | PulseAudio/PipeWire, the sound scheme through libcanberra |
| System | hostnamed, udev, kernel modules, swap, boot loader entries |
| Taskbar and Start Menu | `desktop.ini` |
| User Accounts | AccountsService |

## Taskbar and Start menu

- The taskbar is a layer-shell surface with an exclusive zone, so the work
  area shrinks and maximized windows stop above it. One per output, or only
  on the primary one.
- Task buttons come from `wlr-foreign-toplevel-management`, grouped as XP
  does (always, never, or when full). In Aero the taskbar is Windows 7's:
  pinned and running programs share one row of icon buttons
  (`taskbar-pinned`), with jump lists (recent files from
  `recently-used.xbel` per program, the desktop file's actions).
- Quick Launch is a folder of `.desktop` files or symlinks
  (`~/.local/share/xeondesktop/quicklaunch/`), watched with inotify.
- The notification area hosts StatusNotifierItems (with dbusmenu menus) and
  the built-in clock, volume, network, Bluetooth and battery icons, hiding
  inactive icons behind a chevron as XP does. Legacy XEmbed icons aren't
  supported on Wayland.
- Start menus are built from installed `.desktop` files, recent documents
  and the pinned lists (`start-pinned`, `taskbar-pinned`); the Windows 7
  search also finds Control Panel applets. All three menus work from the
  keyboard.
- The shell plays the menu, device and battery sounds; clicks on its own
  desktop close its menus, since the compositor's popup grab only ends for
  clicks in other programs.

## Memory-safety rules

- Standard formats are parsed with maintained libraries, never by hand:
  GLib's GKeyFile (settings, colour schemes, `index.theme`), GIO's
  GDesktopAppInfo (desktop entries and launching), GBookmarkFile
  (`recently-used.xbel`), GtkIconTheme (icon themes), json-glib (the Qt
  palette); libarchive writes zip files.
- C23, `-Werror` with the warning set in `meson.build` (conversion, sign,
  shadow, cast-qual/align, format=2, VLA ban, ...). GCC and Clang build with
  zero warnings, in debug and optimized builds, and CI checks both.
- Debug builds use ASan and UBSan; `meson test` runs with leak checking.
  `-Danalyzer=true` adds GCC's `-fanalyzer`, also run in CI.
- Hardening: `_FORTIFY_SOURCE=3`, stack protector, stack-clash and CET
  protection, zero-initialised automatics, full RELRO, PIE.
- `strcpy`, `strcat`, `sprintf`, `strtok`, `strncpy`, `gets`, ... are
  `#pragma GCC poison`ed project-wide.
- All size arithmetic goes through `ckd_add`/`ckd_mul`. Allocation failure
  aborts, so there are no unchecked NULLs.
- Strings are length-carrying `xd_str` slices. Parsers never trust input
  lengths and never write through caller-supplied buffers without bounds.
- Lifetimes: arenas for per-frame or per-file data; one clear owner for
  everything else; `XD_FREE` nulls the pointer. Signal handlers on
  long-lived objects (D-Bus proxies, NetworkManager, UDisks) are
  disconnected with the window that owns them, and dialogs keep what they
  use alive until they close.

## Testing

- **Unit tests** (`xd-tests`, `meson test`): settings, schemes and theme
  rendering, parsers, file operations, the applets' logic, against
  python-dbusmock services where a system service is involved.
- **UI tests** (`tests/ui/run.py`, `meson test --setup ui --suite ui`):
  each scenario runs a headless compositor in a private mount namespace
  with a scratch home, no system bus and sounds logged rather than played,
  drives it with `xd-inject`, and checks windows (`xd-windows`), files,
  settings and parts of screenshots. Regressions fixed so far each get a
  scenario that fails without the fix.
- **CI**: GCC and Clang debug builds with the unit tests, optimized GCC and
  Clang builds, a GCC `-fanalyzer` build, the UI tests, and Debian, Arch
  and CachyOS packages built and installed in containers.

## Status

Every part described above exists and has been tested headless; TODO.md
lists what's untested on real hardware, the remaining Windows features, and
known gaps (among them a standalone taskbar for other wlroots compositors,
and Qt's file dialogs).
