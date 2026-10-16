# XeonDesktop

A Windows 2000/XP-style desktop environment for Linux (Wayland), written in
strict C23. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design.

## Build

Dependencies: meson, ninja, cairo, pango; wlroots 0.20 (compositor, later
milestone).

    meson setup build          # debug build with ASan/UBSan
    ninja -C build
    meson test -C build

Release build: `meson setup build-release -Dbuildtype=release -Db_sanitize=none`.
Static analysis: `meson setup build-analyze -Danalyzer=true`.

## Run the compositor

Nested in an existing X11 or Wayland session (opens a window), or from a TTY:

    ./build/xd-compositor --style classic|luna|whistler [--scheme FILE] [--scale 1.5] -s alacritty

| Keys                | Action                                          |
|---------------------|-------------------------------------------------|
| Alt+Tab / Alt+Shift+Tab | Switch windows (release Alt to choose)      |
| Alt+F4              | Close window                                    |
| Win+Up / Win+Down   | Maximize / restore or minimize                  |
| Ctrl+Alt+T          | Open terminal (`--terminal CMD`)                |
| Ctrl+Alt+Backspace  | Quit the compositor                             |

Mouse: drag the caption to move, drag edges/corners to resize, double-click
the caption to maximize, double-click the window icon to close.

## Preview themes

    ./build/xd-themeview --style classic|luna|whistler [--scheme data/schemes/X.ini] [--scale 1.5] --out preview.png
