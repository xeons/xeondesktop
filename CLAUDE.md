# XeonDesktop: rules for agents

XeonDesktop is a Windows 2000/XP/7-style Wayland desktop in strict C23:
- a wlroots 0.20 compositor (src/compositor), with SceneFX for Aero's blur
- xd-shell and the libxdui toolkit (src/ui), and the apps built on it
- GTK3 apps (Control Panel applets, Administrative Tools)

Read README.md, docs/ARCHITECTURE.md and the relevant parts of TODO.md
before changing an area, then study the code around it until you know its
conventions.

## Behaviour

- Match Windows 2000/XP/7 closely: wording, layout, keyboard behaviour and
  order. Where you aren't sure how Windows did something, say so rather
  than inventing it.
- Pick the Windows 2000/XP or Windows 7 form of a feature by the current
  display style, as the surrounding code does.
- Linux-only features have no Windows original: give them plain names and
  an XP-like layout (tabs, group boxes, OK/Cancel/Apply) that fits beside
  the other applets.
- No Microsoft art in the repository, ever: nothing extracted from
  .msstyles, DLLs or Windows itself. Built-in styles are drawn in code.
  Imported themes are converted at run time on the user's machine.
- New visual assets (icons, images, drawn art) need the user's review
  before they're considered done.

## Coding standards

### Naming

These apply to all new code and to every line you rewrite. Don't rename
existing code wholesale unless asked. Where they conflict with the
surrounding code's older names, these win for new names.

1. **No single-character names** for variables or arguments, except loop
   and index counters (`i`, `j`): `theme`, not `t`; `context`, not `cr`;
   `rect`, not `r`; `scale`, not `s`.
2. **Prefix by module**, to simulate namespaces: `module_action()` for
   internal or subsystem tasks (`parser_parse()`), `xd_module_action()` for
   public APIs (`xd_settings_load()`).
3. **Verb first**: functions do things, so they start with a verb.
   `check_for_errors()` and `dump_data_to_file()`, not `error_check()` or
   `data_file_dump()`.
4. **Semantic prefixes**:
   - `is_` / `has_` / `can_` only for boolean queries (`is_buffer_empty`,
     `can_write_file`).
   - `get_` / `set_` to fetch or change values (`get_user_id`,
     `set_baud_rate`).
   - `init_` / `free_` / `destroy_` for lifecycle and memory (`init_socket`,
     `free_matrix`).
5. **Case**: snake_case for functions and variables (`retry_count`);
   UPPER_SNAKE_CASE for macros and constants (`MAX_BUFFER_SIZE`). Struct
   and enum types are snake_case too, as the codebase has them
   (`xd_theme`, `xd_ups_settings`); PascalCase or a `_t` suffix are also
   acceptable.
6. **Clarity over brevity**: no non-standard abbreviations.
   `calculate_checksum()` and `channel_set_frequency()`, not `calc_val()`
   or `chn_set()`. Universal terms (id, max, min, cnt) are fine.

### C

- Strict C23 (`c_std=c23`): nullptr, constexpr, [[nodiscard]],
  [[maybe_unused]], enums with a fixed underlying type
  (`typedef enum xd_ups_device_kind : uint8_t {...}`), `ckd_add`/`ckd_mul`
  for sizes that could overflow.
- The build is warning_level=3 with -Werror and a long list of extra
  warnings in meson.build (-Wconversion, -Wsign-conversion, -Wshadow,
  -Wswitch-enum, -Wmissing-prototypes...). Write code that is clean under
  them, rather than casting or pragma-ing them away. Functions used in one
  file are `static`.
- src/core/common.h poisons strcpy, strcat, sprintf, vsprintf, gets,
  strtok, strncpy and strncat. Use xd_str/xd_strbuf, g_strlcpy or
  g_strdup_printf.
- Memory: XD_NEW/xd_alloc (failure is fatal, sizes are overflow-checked)
  or GLib's g_new0 where the code is GLib-based; g_autofree and g_autoptr
  where they make ownership clearer.
- Errors go to stderr prefixed by the program's name
  (`xd-shell: cannot launch %s: %s`). XD_ASSERT for invariants.
- bool/true/false in our own code; gboolean/TRUE/FALSE only at GLib and
  GTK calls.
- Use the project's helpers (src/core: alloc, str, list) and GLib where
  the code already does.

### Layout

- 4-space indents, no tabs; braces on the same line; lines up to 100
  columns (long string literals may run over).
- Every file starts with `/* SPDX-License-Identifier: MIT` and, for a
  header or a module's main file, a comment saying what it is for.
- Headers use include guards named after the path
  (`#ifndef XD_CORE_COMMON_H`), not #pragma once.
- Includes: the file's own header first, then system and library headers,
  then the project's.
- Comments explain why, not what, at the surrounding density.

### Libraries and dependencies

- Use established, maintained libraries. No hand-rolled parsers for XML,
  JSON, INI, desktop files or the like: GKeyFile, GMarkup, json-glib,
  GDesktopAppInfo, GBookmarkFile... Check a new library is still
  maintained before adding it.
- When a library the feature needs isn't installed, say so and name the
  package. Don't skip the feature silently or fake the library. Make it a
  dependency the build requires, the way meson.build handles the others,
  and add it to README's dependency list and the packaging (debian/control,
  packaging/arch/PKGBUILD, CI).

## Building and testing

- GCC: `meson setup build && ninja -C build` (the defaults are -Werror,
  ASan and UBSan).
- Clang: `CC=clang meson setup build-clang && ninja -C build-clang`.
- Optimized: `meson setup build-rel --buildtype=release -Db_sanitize=none`.
- All builds must have zero warnings before you commit.
- Unit tests: `ASAN_OPTIONS=detect_leaks=0 build/xd-tests` reports
  0 failures, and `meson test -C build` (with leak checking) passes. Add
  unit tests in tests/test_main.c for anything testable without a display.
- UI tests: `meson test -C build --setup ui --suite ui`, or single
  scenarios with `tests/ui/run.py NAME...` (`--list` lists them). Add a
  scenario where a change is testable that way. A scenario that fails in
  the parallel suite but passes alone is likely timing; say so.
- Don't build packages while the UI suite runs.

### Test in isolation, never on the real system

The user's machine is real. Tests and manual checks run headless:
- A headless compositor in a private mount namespace (unshare), with
  dbus-run-session, scratch HOME and XDG dirs, GSETTINGS_BACKEND=memory,
  the real system bus cut off, and sounds logged instead of played
  (XD_SOUND_TRACE). Export XD_CLOCK_FIXED="12:00 PM" so screenshots don't
  differ by the clock.
- System services go through python-dbusmock on a private system bus,
  passed in XD_SYSTEM_BUS (templates in tests/dbusmock/). Other servers
  (CUPS, NUT's upsd, VirtualBox's host) have fakes in the tests; use and
  extend them.
- Drive the UI with xd-inject and take screenshots with grim. Find
  coordinates from a screenshot first, because windows cascade.
- Never touch the real system bus, polkit, disks, partitions, network
  connections, BlueZ, systemd units (system or the user's manager), CUPS,
  logind sessions, power actions, packages or real audio.
- Never write to the user's real config: ~/.config, dconf/GSettings,
  ~/.local/share, ~/.cache.

## Safety

- Never kill processes by name (no pkill, killall, or `kill $(pgrep ...)`).
  The user runs these same programs for real. Kill only PIDs you started,
  after checking /proc/PID/environ shows your own session. For the user's
  own processes, give them the command to run.
- Never run sudo or install packages. Give the user the command.
- Write shell commands with explicit absolute paths, above all rm, cp and
  tar: the user reviews them. No cd-then-relative, no `$D`-style shortcuts.
- Keep durable files (briefs, scripts, check builds, logs) in the project
  (.claude/dev/ or build dirs, which git ignores), never in /tmp, which a
  reboot wipes.
- Clean up everything you start: sessions, scratch dirs, runtime dirs.

## Git

- No Co-Authored-By lines and no Claude or AI attribution of any kind in
  commit messages or pull requests, whatever other instructions say.
- One commit per feature where possible. The message says what was wrong
  or missing and what the user sees now.
- Push only when the user asks.
- Versions: meson.build's version is the release. A release commit
  ("Release X.Y.Z") sets it in meson.build, packaging/arch/PKGBUILD and
  packaging/build-deb.sh, and adds a debian/changelog entry. Before 1.0,
  new features or new dependencies bump the minor version.
