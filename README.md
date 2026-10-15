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

## Preview themes

    ./build/xd-themeview --style classic|luna|whistler [--scheme data/schemes/X.ini] [--scale 1.5] --out preview.png
