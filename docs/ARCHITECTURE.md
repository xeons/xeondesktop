# XeonDesktop architecture

A Windows 2000/XP-style desktop environment for Linux, written in C23.

## Goals

- Faithful Windows 2000 / XP desktop: taskbar, Start menu, Quick Launch,
  notification area, optional taskbar grouping, desktop icons.
- Three Start menu layouts: **Windows 2000** (single column with the side
  banner), **Windows XP** (two columns, pinned + recent), **Windows 7**
  (single column with search, jump lists).
- Visual styles as in XP: **Classic** (Windows 2000, with colour schemes),
  **Luna** (XP) and **Whistler** (pre-release look).
- Per-monitor DPI and full multi-monitor support.
- Memory safety as far as C allows (see below).

## Components

Each component is a separate process, so a crash in the taskbar can't take
the session down (the same reason explorer.exe restarts on its own).

| Binary              | Role                                                         |
|---------------------|--------------------------------------------------------------|
| `xd-compositor`     | Wayland compositor + window manager (wlroots 0.20). Draws window frames server-side with the theme engine, handles focus, snapping, Alt+Tab, per-output scale. |
| `xd-shell`          | Taskbar, Start menu, Quick Launch, tray, desktop icons. Uses layer-shell, plus a private `xd-shell-v1` protocol to talk to the compositor. |
| `xd-settingsd`      | Owns the settings store, applies changes live, broadcasts them over D-Bus. |
| `xd-control`        | Control Panel host. Each applet is a module.                 |
| `xd-desk`           | Display Properties (also opened from the desktop's right-click menu). |
| `xd-themeview`      | Developer tool that renders theme previews to PNG.           |

Shared libraries:

- `libxdcore`: checked allocation, arenas, strings, INI, logging.
- `libxdtheme`: colour schemes, metrics, visual styles, DPI-aware drawing.
- `libxdui` (planned): a small Win32-style widget toolkit (buttons, check
  boxes, tabs, list views, tree views, property sheets). It's drawn with
  libxdtheme so every companion app matches the selected style exactly.
  GTK/Qt can't reproduce Classic bevels or colour schemes faithfully.

## Display and DPI model

- All theme metrics are in **logical units** (1 unit = 1 px at 96 DPI).
- Each output has its own scale (1.0, 1.25, 1.5, 2.0, ...). Surfaces are
  rendered at the scale of the output they're on (`wp_fractional_scale_v1`
  + `wp_viewporter`).
- `xd_px()` converts logical units to whole device pixels, never rounding a
  visible line down to 0. That keeps 1-unit bevels crisp at fractional scales.
- Multi-monitor: each output gets its own taskbar (optional, like Windows 8+)
  or only the primary one (like XP). Windows maximize to their own output's
  work area. A frame drawn across two outputs takes the scale of the output
  holding most of the window.

## Theme engine

- **Colour schemes** (`data/schemes/*.ini`) use the Windows registry colour
  names (`ActiveTitle`, `ButtonFace`, ...) plus `[Metrics]` and `[Fonts]`.
  A scheme file only needs to list what differs from Windows Standard.
- **Classic** draws everything from the scheme, so every classic scheme
  (Standard, Classic, High Contrast, Brick, Desert, Eggplant, Lilac, Maple,
  Marine, Plum, Pumpkin, Rainy Day, Rose, Slate, Spruce, Storm, Teal, Wheat,
  plus Large/Extra Large variants) is just a data file. Shipped so far:
  Standard, Classic, High Contrast Black/White. Values written from memory
  need checking against a reference before release.
- **Luna / Whistler** are palettes for one shared rounded-gradient renderer.
  Luna gets Blue / Olive Green / Silver colour variants the same way.
- All artwork is drawn procedurally. No bitmaps from Microsoft products are
  shipped. Users can supply their own fonts (Tahoma, Trebuchet MS); the
  defaults fall back to DejaVu/Noto.

## Display Properties (`xd-desk`)

Five tabs, as in Windows XP:

1. **Desktop / Background**: wallpaper list + browse, position (centre,
   tile, stretch, fill, fit), background colour, per-monitor wallpaper,
   "Customize Desktop" (desktop icons, icon cleanup).
2. **Screen Saver**: saver list with live preview in the monitor picture,
   wait time, "On resume, password protect" (hands off to the lock screen),
   Settings/Preview buttons, and a **Power** button that opens Power Options.
   Savers are plugins drawn on a layer-shell overlay surface. Idle detection
   uses `ext-idle-notify-v1`.
3. **Appearance**: windows and buttons style (Classic / Luna / Whistler),
   colour scheme, font size (Normal / Large / Extra Large), **Effects...**
   and **Advanced** (per-item colours, sizes and fonts, with a live preview of
   active/inactive windows and a message box, like Windows 2000's).
4. **Effects**: menu/tooltip transition (fade / scroll / none), font
   smoothing (none / standard / ClearType-style subpixel), large icons, menu
   shadows, show window contents while dragging, hide underlined access
   keys until Alt is pressed.
5. **Settings**: draggable monitor arrangement, per-output resolution,
   refresh rate, scale (DPI), orientation, primary monitor, "Extend my
   desktop onto this monitor", Identify, Troubleshoot. Changes use
   `wlr-output-management` and roll back automatically after 15 s unless
   confirmed.

## Control Panel (`xd-control`)

Category view (XP) and classic icon view (2000). Applets that make sense on
Linux:

| Applet                    | Backend                                              |
|---------------------------|------------------------------------------------------|
| Display                   | `xd-desk` (above)                                    |
| Taskbar and Start Menu    | `xd-settingsd`: Start style 2000/XP/7, grouping, Quick Launch, auto-hide, clock, tray icon hiding |
| Folder Options            | file manager settings, file associations (`mimeapps.list`) |
| Fonts                     | fontconfig: install, preview, remove                 |
| Mouse                     | libinput through the compositor: speed, acceleration, double-click, buttons, pointer scheme (XCursor themes), wheel |
| Keyboard                  | repeat rate/delay, xkb layouts and options           |
| Sounds and Audio Devices  | PipeWire (`wireplumber` / `pw-cli`), system sound scheme |
| Date and Time             | `org.freedesktop.timedate1` (timedated), NTP         |
| Regional and Language     | `org.freedesktop.locale1`, number/date formats      |
| Network Connections       | NetworkManager D-Bus                                 |
| Power Options             | UPower, `org.freedesktop.login1` (lid/power button), screen blanking |
| User Accounts             | AccountsService, picture, password (via polkit)      |
| Add or Remove Programs    | PackageKit and Flatpak                               |
| Printers and Faxes        | CUPS                                                 |
| Scanners and Cameras      | SANE, gphoto2                                        |
| Bluetooth Devices         | BlueZ D-Bus                                          |
| System                    | general info (CPU/RAM/OS), hostname (`hostname1`), device manager view (udev), performance options |
| Administrative Tools      | services (systemd units), event viewer (journald), disk management (UDisks2) |
| Accessibility Options     | sticky/filter keys, high contrast toggle, cursor size |
| Scheduled Tasks           | systemd user timers                                  |
| Game Controllers          | evdev joystick test/calibration                      |

Each applet is a shared module with a fixed C ABI (`xd_applet_v1`). It runs
inside `xd-control` and makes privileged changes through polkit-guarded
D-Bus services. Nothing runs setuid.

## Taskbar and Start menu

- Task list from the compositor (`xd-shell-v1`, a superset of
  `wlr-foreign-toplevel-management`). Grouping is optional: always, never,
  or when the taskbar is full (XP's rule).
- Quick Launch: a folder of `.desktop` files, drag and drop to reorder.
- Notification area: StatusNotifierItem (KDE/Ayatana) host plus legacy XEmbed
  bridging is out of scope on Wayland. Clock, volume, network and battery
  are built in.
- Start menu content comes from XDG menus (`applications.menu`), recent
  documents (`recently-used.xbel`), and pinned items in settings.

## Memory-safety rules

- C23, `-Werror` with the warning set in `meson.build` (conversion, sign,
  shadow, cast-qual/align, format=2, VLA ban, etc.), clean on GCC and Clang.
- Debug builds use ASan + UBSan. `meson test` runs with leak checking.
  `-Danalyzer=true` adds GCC `-fanalyzer`.
- Hardening: `_FORTIFY_SOURCE=3`, stack protector, stack-clash and CET
  protection, zero-initialised automatics, full RELRO, PIE.
- `strcpy`, `strcat`, `sprintf`, `strtok`, `strncpy`, `gets`, ... are
  `#pragma GCC poison`ed project-wide.
- All size arithmetic goes through `ckd_add`/`ckd_mul`. Allocation failure
  aborts, so there are no unchecked NULLs.
- Strings are length-carrying `xd_str` slices. Parsers never trust input
  lengths and never write through caller-supplied buffers without bounds.
- Lifetimes: arenas for per-frame or per-file data; one clear owner for
  everything else; `XD_FREE` nulls the pointer.

## Milestones

1. Foundations + theme engine (done): core lib, scheme files, Classic /
   Luna / Whistler frames, buttons, check boxes, taskbar pieces, DPI scaling,
   tests, `xd-themeview`.
2. Compositor: wlroots 0.20 scene graph, xdg-shell, server-side decorations
   from libxdtheme, move/resize, focus, per-output scale.
3. Shell: taskbar, Start menu (2000 layout first), Quick Launch, clock.
4. `libxdui` widget toolkit, then Display Properties.
5. Start menu XP and 7 layouts, grouping, tray (SNI).
6. Control Panel host and applets, roughly in table order.
