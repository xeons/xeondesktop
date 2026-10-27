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
| `xd-vboxdnd` | none (Wayland, GLib) | VirtualBox drag and drop in both directions, through the host's HGCM service; started by the compositor in a VirtualBox guest. |
| `xd-mines` | GTK3 | Minefield, XP's mine-clearing game, drawn with cairo. |
| `xd-snip` | GTK3 | The Snipping Tool (its overlay through gtk-layer-shell), Print Screen's clipboard copies, and screen recording (GStreamer). |
| `xd-help`, `xd-defaults` | GTK3 | Help and Support, Set Program Access and Defaults (the Start menus' Search is `xd-files --search`). |
| `xd-control` | none | Opens `control:` in `xd-files`, or one applet (`xd-control desk`). |
| `xdg-desktop-portal-xeondesktop` | GTK3 | The xdg-desktop-portal FileChooser backend: our Open and Save As dialogs (XP's layout with 7's search box, around the folder view) for Flatpaks, GTK 4, Firefox, Chromium and GtkFileChooserNative. In libexecdir, D-Bus activated. |
| `xd-access`, `xd-mouse`, `xd-keyboard`, `xd-sound`, `xd-power`, `xd-timedate`, `xd-intl`, `xd-folders`, `xd-fonts`, `xd-users`, `xd-sysdm`, `xd-netconn`, `xd-bluetooth`, `xd-joy`, `xd-effects`, `xd-printers`, `xd-scanners`, `xd-appwiz`, `xd-schedtasks`, `xd-eventvwr`, ... | GTK3 | Control Panel applets and Administrative Tools, one program each. |

Development tools: `xd-themeview` (renders theme previews to PNG),
`xd-inject` (virtual pointer and keyboard, touch, pen and touchpad
gestures for UI tests), `xd-test-input` (a window that logs the input
reaching it), `xd-windows` (lists windows for UI tests), `xd-paste` (reads the
clipboard for UI tests), `tools/luna-measure.py` (measures Luna
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
with Qt's Windows style for Classic and Fusion otherwise. With a theme
package, the theme draws GTK widgets with the package's own images,
rendered into PNGs beside the CSS and stretched by GTK as border images
(docs/THEMES.md, "GTK and Qt programs"). The user can pick
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
  15-second revert; its adaptive sync is the monitor's variable refresh
  rate setting, and Advanced asks with a test configuration whether the
  monitor can have it);
- `linux-drm-syncobj-v1` (explicit sync, which NVIDIA's driver needs; only
  when the renderer and backend both do DRM timelines, as sway decides) and
  `tearing-control-v1` (games with vsync off). wlroots 0.20 has no
  `fifo-v1` or `commit-timing-v1`, so neither is offered;
- `wlr-gamma-control` (gammastep, wlsunset: their ramp replaces the
  compositor's own while they hold it);
- `wp_fractional_scale_v1` and `wp_viewporter`, `wp_cursor_shape_v1`,
  `xdg-activation-v1`;
- `xdg-foreign` v1 and v2: portal dialogs (the file chooser) owned by the
  window of the program that asked;
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
- `xd-drag-relay-v1`, our private protocol, offered only to the
  `xd-vboxdnd` the compositor started: drags it starts without a button
  held and moves where the VirtualBox host says, and the drags programs
  here start, told to it so it can carry them out to the host;
- `wlr-screencopy` and `xd-window-rectangles-v1`, our protocol saying
  where the windows are, front to back (the Snipping Tool's Window Snip
  and Alt+Print Screen), for every program that isn't sandboxed;
- `zwp_tablet_manager_v2` (drawing tablets' pens and pads) and
  `zwp_pointer_gestures_v1` (touchpad swipes, pinches and holds) for
  every program;
- `pointer-constraints`, `relative-pointer` and
  `keyboard-shortcuts-inhibit` for programs that capture the mouse and
  keyboard (games, Blender, virtual machines, remote desktops), and
  `xwayland-keyboard-grab`, which wlroots lacks and the compositor
  implements, offered to Xwayland alone (see Compositor, Input capture);
- `wlr-virtual-pointer` and `virtual-keyboard`, only with
  `--allow-virtual-input` (UI tests), and with it `xd-test-input-v1`, our
  test protocol: a touchscreen, a tablet with pen and pad, and a touchpad
  made as real wlroots devices, fed by `xd-inject`, since the headless
  backend has no such devices and uinput would reach the real machine.

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
  XP's activation shortcuts. Ctrl+Alt+F1..F12 switch virtual terminals.
- **Touch and pens** (`touch.c`, `tablet.c`). A finger on a surface whose
  program has `wl_touch` goes to it as touch. Anywhere else it works the
  pointer (one finger at a time), as Windows 7 promotes touch to mouse
  messages for programs that don't ask for touch: so our frames (caption
  buttons, moving, sizing, Aero Snap), the shell (libxdui reads only the
  pointer) and the locker work by touch without code of their own. A
  program that moves its window from a touch (`xdg_toplevel.move` with the
  touch's serial) has the finger drive our move. A pen goes to programs
  that bound tablet-v2 on our seat as a pen (pressure, tilt, buttons; the
  pad's focus follows it); to the rest as the pointer, its tip the left
  button and barrel buttons right and middle. Which way is decided while
  the pen hovers and kept for a whole stroke. Each touchscreen and pen
  display is mapped to its monitor (udev's `WL_OUTPUT`, else the built-in
  `eDP`/`LVDS`/`DSI` panel, else the first), other tablets to the whole
  desktop, again whenever monitors change; libinput applies the
  calibration matrix and wlr_cursor the monitor's rotation.
- **Touchpad gestures** (`gestures.c`) go to the program under the
  pointer; the compositor keeps none for itself.
- **Input capture** (`constraints.c`). A program may lock the pointer (it
  then gets only relative motion, raw and accelerated, which every motion
  sends anyway) or confine it to its window or a region of it, as sway and
  labwc do: only for the window with the keyboard, from when the pointer
  is over it (in the region); it ends when the window loses the keyboard
  and while our own grabs run (moving and sizing by mouse or from the
  window menu, a drag, the lock screen). A lock that ends puts the pointer
  at the program's cursor hint. Absolute motion (a tablet, a nested
  session, MouseKeys) goes through the same path, as a motion from where
  the cursor is. A program may also take our shortcuts
  (`keyboard-shortcuts-inhibit`; for X11 programs, Xwayland's keyboard
  grab, which it asks for on XGrabKeyboard, also for a menu of the window
  with the keyboard): while its window has the keyboard, the Windows key,
  Alt+Tab, Alt+F4, Alt+Space, Ctrl+Esc, Ctrl+Shift+Esc, Ctrl+Alt+T and
  the Win+ keys go to it. Ours still: Ctrl+Alt+Delete (the secure
  attention sequence, which no Windows program can take), Win+L (Winlogon's
  in Windows), Ctrl+Alt+F1..F12 and Ctrl+Alt+Backspace (the session's),
  the power and sleep keys and the accessibility shortcuts (`grabkeys.h`).
  Inhibitors are always honoured, as in sway: Windows asks no permission
  for a low-level keyboard hook either, and one works only while the user
  keeps its window in front. Nothing strands the user: Ctrl+Alt pressed
  and let go on its own (VMware's key) gives back the pointer and the
  shortcuts until the window is clicked or gets the keyboard again; any
  shortcut of ours that the program didn't take (Alt+Tab, the Windows key,
  Ctrl+Esc, Win+arrows) releases the pointer the same way, and
  Ctrl+Alt+Delete always reaches us. X11 programs get these through
  Xwayland: a pointer grab with `confine_to` becomes a confinement, and
  with no cursor shown, Xwayland locks the pointer and moves its own
  (pointer warping, as X11 games use it).
- **Gamma.** Each monitor's gamma ramp combines its calibration (the
  vcgt curves of its ICC profile, read with Little CMS) and the night
  light (a black-body white point by Tanner Helland's fit, scheduled by
  NOAA's sunrise and sunset equations or set hours, changing over half
  an hour). The ramp goes into a frame's commit and is loaded by KMS, so
  it works with pixman too; headless and nested outputs have none.
- **Games** (`present.c`). A full-screen window that's alone on its output
  is scanned out directly by the scene graph: fullscreen windows have no
  frame, blur or shadow nodes, sit above the taskbar (whose glass is then
  hidden under an opaque window and culled), and the cursor is a hardware
  plane except on virtual GPUs. Explicit sync works on both scene graphs:
  wlroots' and SceneFX's wait for a buffer's acquire point when drawing or
  scanning it out and signal its release point, and our own thumbnail
  capture (`capture.c`) does the same. Variable refresh rate is per
  monitor (`AdaptiveSync` in `monitors.ini`: off, fullscreen, always), off
  by default; "fullscreen" turns it on in the frame's commit while a
  full-screen window is in front (as Windows uses it for games) and off
  again on the desktop, and a monitor that refuses it isn't asked again
  until the setting changes. A full-screen window's tearing-control async
  hint flips without waiting for vblank (`tearing_page_flip`) when the
  frame is its own buffer (direct scanout), never in the commit that
  changes adaptive sync, with a test commit first and a vsynced flip if the
  driver refuses. No setting gates tearing: the program's hint is the
  user's choice in that program, as with DXGI's ALLOW_TEARING. The
  decisions are in `present.h`, unit-tested.
- **Session.** In a real login the compositor gives the systemd user
  manager the display (`WAYLAND_DISPLAY`, `DISPLAY`, `XDG_*`, the theme
  variables) and starts `xeondesktop-session.target`, which binds
  `graphical-session.target` and pulls in `xdg-desktop-autostart.target`,
  so autostart entries and "When I log on" scheduled tasks run.
  `xeondesktop-session` stops it when the compositor exits. Nested and test
  sessions never touch the user manager. A drop-in makes VirtualBox's
  guest client share the clipboard as a Wayland client.

## Desktop Effects

Compiz Fusion's effects, configured in Desktop Effects (`xd-effects`, laid
out as CompizConfig Settings Manager was). Every effect is off by default:
out of the box the desktop behaves as Windows does.

**The table** (`src/settings/effects.h`, in `libxdsettings`). Each effect
is an `xd_effect_info` in a file of its own under `src/settings/effects/`
and one `XD_EFFECT(name)` line in `src/settings/effects_list.h`: its id
(its group in the file), name, category (CCSM's: Effects, Window
Management, Desktop, Accessibility, Utility), one-line description, icon
name, whether it's drawn with OpenGL, and its options. An option has a
key, a label, an optional tooltip and tab (`group`), a type (bool, int or
double with a range, colour, choice with labels, key binding, mouse button
binding, screen edges and corners, window match) and a default in the
file's syntax. The panel builds every page from the table, so a new effect
needs no panel code.

- Settings: `xd_effects_create_settings()`, `xd_effects_load()`,
  `xd_effects_save()`; `xd_effects_is_enabled()`, `xd_effects_get_value()`
  and `xd_effects_set_value()` (clamped) by index; `xd_effects_get_bool()`,
  `_int()`, `_double()`, `_color()`, `_choice()`, `_edges()`, `_text()` by
  key, for the compositor's effects.
- `xd_effects_find_conflicts()`: an effect's bindings and edges that clash
  with those of the effects turned on (`xd_effects_bindings_overlap()`
  compares two values; `xd_effects_parse_binding()` reads GTK's
  accelerator syntax).
- `xd_effects_match_window()`: the window-match syntax (`app_id=`,
  `title=`, `type=normal|dialog`, `!`, `&`, `|`, globs).

**The file.** `~/.config/xeondesktop/effects.ini`, a GKeyFile like
`monitors.ini` (desktop.ini has its own parser): one group per effect,
`Enabled=` and the options that differ from their defaults; `[Preferences]
WarnConflicts`. Values are checked against the table (out of range:
clamped; unparsable: the default), and saving keeps groups, keys and
comments this build doesn't know. The panel saves each change at once; the
compositor watches the folder (inotify, as `desktop.ini`) and applies it.

**The support file.** The compositor writes
`$XDG_RUNTIME_DIR/xeondesktop/effects-$WAYLAND_DISPLAY.ini` at startup
(`OpenGL=`, `Reason=`, `Renderer=`) and removes it at exit; the panel greys
out the OpenGL effects with the reason when it says so, or when there's
none.

**In the compositor** (`src/compositor/effects/`). `effects.c`, in both
builds, loads and watches the settings and turns each `xd_effect_impl` on
and off; an implementation is a file of its own and one
`XD_EFFECT_IMPL(name)` line in `impl_list.h` (inside `#ifdef XD_SCENEFX`
for OpenGL ones, whose files are `effects_fx_src` in meson.build). Its
hooks, all optional: `enable` (returns its state), `disable`, `configure`
(its options, after enable and on every change), `window_event` (map,
unmap, destroy, focus, maximize/restore/snap with the box before,
minimize, unminimize, title; sent from `view.c`) and `frame` (before each
output's frame, Compiz's preparePaint), `input` and `animates` (below).
The plain `xd-compositor` compiles none of the OpenGL ones and reports
them unavailable.

**Input.** Effects have one way in, the `input` hook: `input.c` offers
the effects that are on an `xd_effect_input` (`xd_effects_input()`, one
call per kind of event) before its own bindings and the programs see it.
Keys (presses and releases), buttons (presses and releases) and steps of
the vertical wheel come while no window is being moved or sized, never
while the screen is locked, and keys not while a program takes the
shortcuts; every pointer move and modifier change comes always, with the
event's time. The effects are asked in `impl_list.h`'s order. An effect
returns true to use it up: keys, buttons and wheel steps then go no
further (a used-up press's release goes nowhere else either, and a
Windows+click opens no Start menu), while moves and modifier changes
reach every effect (a used-up move doesn't reach the windows: the pointer
stays with an effect holding it). Options of the KEY and BUTTON types are
read once with `xd_effects_get_binding()` and matched with
`xd_effects_binding_matches()`: exactly the modifiers held (as
`xd_binding_modifier` bits), keys in lower case and Shift+Tab as Tab, and
Button4 and Button5 as the wheel's steps up and down, as in Compiz.
`xd_effects_is_binding_held()` says whether a binding's modifiers are
all still held, for switchers that end when they're let go.

**Who animates a window.** `animates(view, kind)` says an effect animates
that window event itself. For a minimize or unminimize `view.c` then
skips XP's bar animation (`animate.c`). Effects that animate events
others may too ask `xd_effects_is_animated_before()`, and leave the
window to an effect before them in `impl_list.h` that does: with Magic
Lamp and Animations both on, Magic Lamp (one of the choices of Compiz's
Animation plugin) minimizes and restores the windows it takes, and opens
and closes them with OpenClose, and Animations the rest, never both at
once.

**Scene-graph effects** (both builds) draw with scene nodes only: buffers'
opacity and destination size, rects, positions. Helpers:
`xd_effects_create_overlay()` (a tree above every window and panel, below
the lock screen), `xd_effects_schedule_frames()` and
`xd_effects_match_view()`. A **window mirror** (`mirror.h`) is a window's
picture somewhere else: a tree of scene buffers showing the buffers of
the window's frame, surfaces, subsurfaces and popups, copied from its own
nodes by `xd_mirror_update()` (each frame, re-set only when the window
committed or a buffer changed), placed and scaled with
`xd_mirror_place()`. Its scene buffers hold their buffers, so once
detached (`xd_mirror_detach()`, the window is closing) it keeps the last
picture. A window hidden behind its mirror gets frame events from
`xd_mirror_send_frame_done()`, so the mirror stays live.

**Scale** (`scale.c`, the maths in `scale_model.c`, unit-tested) is
Compiz's window picker: Compiz's slot layout (about √n rows, each window
in the free slot nearest it) and its velocity model for the glide
(`speed`, `timestep`). The windows are hidden and their mirrors glide over
darkened outputs; it has the keyboard and pointer until a window is picked
(click, Enter) or it's cancelled (Esc, a click on no window, its key).

**Animations** (`animations.c`, the curves in `animations_model.c`,
unit-tested) is Compiz's animation plugin for open, close, minimize and
unminimize: Fade, Zoom, and Glide 1 and 2 (their 3D tilt drawn as a flat
squash), each event with its effect, duration and windows. The window's
mirror is placed just above it in the stacking order and animated; an
appearing window is hidden till it's over. A closing window's mirror is
detached at unmap, keeping its last picture. Minimizing zooms to the
taskbar button the shell reported (`button_box`, as `animate.c` uses), and
`animates` stops XP's bar for the windows it animates.

**Window Opacity** (`opacity.c`) is the opacity part of Compiz's Opacity,
Brightness and Saturation: a window's buffers are drawn at its first
matching rule's opacity, or as Alt and the wheel over it last set, and
inactive ones no more than the inactive opacity if dimmed. Neither scene
graph (wlroots' or SceneFX's) draws a buffer with a brightness or
saturation, so those aren't offered. The opacities are set again before
each frame (an unchanged one costs nothing), which also restores them
after an OpenGL effect or Aero Peek, whose own are left alone while it
shows.

**Ring Switcher** (`ring.c`, the ellipse in `ring_model.c`, unit-tested)
is Compiz's ring plugin, beside Alt+Tab rather than in its place: the
windows' mirrors on an ellipse over the darkened monitor, smaller and
fainter towards the back, turned with Scale's velocity model (in degrees)
to the one picked, its title drawn with the theme's caption font below.
Its key's modifiers (but Shift) keep it open; letting go switches. Its
Previous window key is None by default (Win+Shift+Tab is Shift
Switcher's); Shift+Tab goes back while it shows.

**Enhanced Zoom Desktop** (`zoom.c`, the view in `zoom_model.c`,
unit-tested) is Compiz's ezoom. Neither scene graph scales an output's
view, so a zoomed output's frame is drawn twice: `wlr_scene_output_build_state()`
into a swapchain of the effect's own (its node hidden), then that buffer,
cut to the view (`src_box`) and stretched over the output, by a scene
buffer at the very top of the scene, which the output's own frame shows.
The first drawing happens only when the scene needs a frame or the view
moved, so a still screen costs nothing more; direct scanout is off
meanwhile (`wlr_output_lock_attach_render`). The view keeps the point
under the pointer under it, so input needs no translating. Software
cursors are drawn in both drawings, so the zoomed picture has a larger
copy of the pointer under the real one, as ezoom's scaled pointer.

**Annotate** (`annotate.c`) is Compiz's annotate plugin, freehand: a
cairo picture per output in an overlay that takes no input. Each output
keeps the strokes finished apart from the picture shown, which is redrawn
from them with the stroke being drawn (its outline, then its line), so a
stroke's outline never covers its own line. While a stroke is drawn it
has the pointer; the windows keep the keyboard throughout.

**Show Mouse** (`showmouse.c`) is Compiz's showmouse simplified: in
place of particles thrown from turning emitters, the emitters alone, a
ring of glowing dots turning round the pointer, a small cairo picture
drawn again each frame while it shows.

**Virtual desktops** (`desktops.c`, the grid's maths in `desktops_grid.c`,
unit-tested). General Options (`always_on` in the table: no Enabled=, no
check box) sets how many desktops there are across and down, 1 x 1 by
default, and the keys that switch (Ctrl+Alt+arrows) and carry the active
window along (Ctrl+Alt+Shift+arrows). Each window has a desktop
(`view->desktop`, its owner's for a dialog) or is on all (`view->sticky`);
switching disables the other desktops' window trees and destroys their
foreign-toplevel handles, so the taskbar, its thumbnails, Alt+Tab,
Cascade and Tile see only this desktop's windows (and remakes the
handles, in the order the windows opened, coming back). Focusing a window
elsewhere shows its desktop. The shell hears of the desktops and each
window's (xd-window-arranger-v1's `desktops` and `window_desktop`), and
asks to switch (`switch_desktop`, the taskbar's pager) or to move a
window (`move_window_to_desktop`, `set_window_sticky`: the window menu's
Move to Another Workspace and Always on Visible Workspace). Effects
animate a switch through `xd_desktops_add_animator()`: the desktop is
current already, and the effect shows the move from the old one.

**Desktop Wall** (`wall.c`, both builds; its maths, with the other
desktop effects', in `viewport_model.c`, unit-tested) is Compiz's wall
plugin: a switch moves the scene nodes of the old and new desktops'
windows by the layout's size, eased, for the slide (the windows stay
where they are), with a map of the grid drawn with cairo in `layer_drag`.
Edge flip move (off by default): a window dragged against the layout's
edge for the delay is carried to the desktop that way, the pointer
warped to come in from the other side.

**Whole-output OpenGL effects** (`glfx_output.c`, declared in `glfx.h`).
`xd_glfx_capture_scene()` draws what the scene shows of a box into a
texture, walking the scene tree itself so a filter can pick windows
whether they're shown or not (another desktop's, minimized): the desktop
colour, layer surfaces and those windows, without `layer_drag`, the lock
screen or the effects' own pictures; while any capture object exists,
every window's frame keeps its pixels. `xd_glfx_begin_output()` starts an
effect that draws whole outputs: from its `frame` hook it captures what it
needs and calls `xd_glfx_draw_output()`, whose `draw` paints the output
into a buffer shown by an opaque scene node under the lock screen; the
effect drives its frames with a timer and takes the pointer through the
`input` hook. `xd_glfx_draw_quad()` (a texture on a quad in clip space with
its w, for perspective) and `xd_glfx_fill_polygon()` draw in 3D; the
maths stays on the CPU (`viewport_model.c`'s camera: Compiz's 60 degree
field of view, the screen filling the view at z = 0). `XD_EFFECTS_TIMING=1` logs
their frame times too (capturing and drawing).

**Desktop Cube** and **Rotate Cube** (`cube.c`): the current row's
desktops are the faces of a prism (two are two sides of a cube with blank
sides), each face a scene capture of its desktop, back faces culled (or
drawn first when the cube is see-through while turning), caps filled. A
switch across turns it (above Desktop Wall's priority, which still
slides up and down), zooming out half-way; Rotate Cube's binding
(Ctrl+Alt+Button1) drags it round and tilts it, and let go it settles on
the nearest desktop face and switches there without animating again.

**Expo** (`expo.c`): every desktop as a wall (Compiz's expo, flat), each a
scene capture at about the size it's drawn, zoomed from the current one
(`xd_expo_get_cell()`). Its key (Win+W) or corner (top left) shows it;
while it shows, the pointer and clicks are its: a click zooms in to that
desktop (switched to at once, with the highest animator priority so
nothing else animates it), a press on a window and a drag carries it (a
capture of its own, following the pointer) to another desktop.

**Shift Switcher** (`shift.c`, Window Management): Compiz's shift, Cover
mode by default (Flip too). Its key (Win+Shift+Tab; Compiz's Win+Tab is
Ring Switcher's) shows this desktop's windows, most recently used first,
each a scene capture of its own drawn in perspective with a reflection
(`xd_shift_get_slot()`), over the dimmed desktop; the key again or the
arrows step; letting go of the binding's modifiers
(`xd_effects_is_binding_held()`), Enter or a click activates the one in
front, Escape changes nothing.

**The OpenGL path** (`glfx.h`, `xd-compositor-fx` only). A window effect
takes a window over with `xd_glfx_begin_window(view, impl, data)`; each
frame, from the output's frame event before the scene is drawn:

1. the window's subtree (frame, surfaces, subsurfaces, popups) is drawn
   into an offscreen buffer, `effect->texture` (`box`: where it is, layout
   units; `u_scale`/`v_scale`: the part of the texture it fills);
2. `begin` (the first frame) and `update(effect, seconds)` (false: over)
   run, and `get_bounds` says where it will draw;
3. `draw(effect, draw)` draws into a buffer covering those bounds with
   GLES2 calls of its own, inside a pass of SceneFX's renderer (its context
   current, the buffer bound; `draw->projection` maps layout units to it);
4. that buffer is shown by a scene buffer node at the top of the window's
   own tree, while the window's own buffers are drawn at opacity 0 and its
   blur and shadow are off.

So the picture is stacked where the window is (windows above stay above,
raising it raises the picture, a fullscreen window's moves with it) and
input still reaches the window where it really is; the scene graph damages
what the node covered and covers. While any window effect runs, direct
scanout is off on the outputs (`wlr_output_lock_attach_render`) and frames
keep coming from their frame events until every effect is done; `done`
frees the effect's data. Helpers: `xd_glfx_init_mesh()` (a grid of
vertices with texture coordinates, `xd_glfx_set_mesh_rect()`,
`xd_glfx_get_mesh_bounds()`), `xd_glfx_draw_mesh()` (the built-in shader)
and `xd_glfx_draw_mesh_with()`, `xd_glfx_init_shader()` (GLSL ES 1.00,
compiled for 2D or external textures: `XD_SAMPLER`), `xd_glfx_set_ortho()`
and `xd_glfx_multiply()`. The frame's pixels are kept for the capture while
an effect runs (`xd_frame_keep_pixels`): the scene graph lets go of a
buffer once it has a texture. Buffers are whole multiples of 64 px
(llvmpipe overruns smaller udmabuf buffers). `XD_EFFECTS_TIMING=1` logs
frame times.

**Closing effects** (close, minimize). `xd_glfx_begin_closing_window(view,
impl, data)`, called from the UNMAP or MINIMIZE window event while the
window's content is still there, draws the window once into the effect's
texture, which it keeps (`held`): the effect runs on without the window,
its node beside the window's tree in the same layer (just above it), so
it stays where the window was stacked when the tree is hidden or freed.
`effect->view` is the window until it's destroyed (`xd_glfx_drop_view`),
then nullptr; a live effect begun on a window that has a closing one
(restored while it's being minimized) ends it. An effect that draws
minimizing and restoring says so in its `animates` hook, as any effect
does.

**Output filters** (Water: a post-process over the finished picture).
`xd_glfx_add_output_filter(server, impl, data)`: while a filter is active,
output.c asks `xd_effects_build_output_state()` for each frame, which has
the scene graph draw into a buffer of ours (`wlr_scene_output_build_state`
with our swapchain, so its damage tracking still holds) and the filter's
`draw(filter, output, scene, draw)` draw the output's real buffer from it
(`scene->box` the output's box in layout units, `draw->projection` from
layout units). Several filters run in the order added, each from the one
before. `update(filter, seconds)` runs once a frame and says whether it
changes the picture: an idle filter costs nothing (no pass, no frames)
till `xd_glfx_wake_output_filter()`, and going idle has the scene graph
repaint the whole output. Outputs rotated or flipped aren't filtered.

**A frame, and the two whole-output paths.** Output filters and
whole-output effects (`glfx_output.c`) both change a whole output but do
different things, so both stay: a whole-output effect (Cube, Expo,
Shift) draws the output from captures of the scene into a scene node,
which the scene graph then shows like any other (the lock screen and the
dragged things above it, input going where it would); a filter works on
the finished picture, after the scene graph drew it. `output.c`'s frame
runs them in one order: (1) `xd_effects_output_frame()`, the effects'
frame hooks in `impl_list.h`'s order (whole-output effects capture and
draw; Enhanced Zoom, last, draws the scene again magnified, so it zooms
the cube or Expo too), then the OpenGL window effects; (2)
`xd_effects_build_output_state()`, the scene graph's picture and the
filters over it (Water ripples the cube too); (3) the gamma ramp
(`gamma.c`) and present options set on the same commit. The ramp is a
colour transform the display applies to whatever buffer is committed,
so filtered frames keep Night Light. Scene captures leave out every tree
an effect tags as its own picture (`xd_effects_tag_picture()`:
whole-output effects', Zoom's and the overlays), so a desktop's picture
is the desktop's.

**Wobbly Windows** (`wobbly.c`, the model in `wobbly_model.c`, unit-tested)
is Compiz's wobbly plugin: a 4 x 4 grid of masses joined by springs, the
grabbed one held at the pointer, drawn as the bicubic Bézier patch they
are the control points of (exactly the rectangle at rest), in steps of
15 ms. Let go, each mass is also drawn to its place on the window, so it
settles where the window is (Compiz moved the window instead).

**Magic Lamp** (`magic_lamp.c`, the shape in `magic_lamp_model.c`,
unit-tested) draws a minimized window (a closing effect) as a mesh of
strips that first bend toward its taskbar button along a half cosine and
then slide into it, squeezed to the button's size; worked out for a
button below the window and turned for any other edge. Restoring runs it
backwards as a live effect (from where a minimize still under way had
got to). With OpenClose, windows open from their button or the pointer
and close into it. It's our own curve, not Compiz's (no waves).

**Burn** (`burn.c`, the front and the sparks in `burn_model.c`,
unit-tested) burns a closing window's picture away: a shader moves a
front across it (ragged with value noise), charring the band behind it,
glowing in the fire's colour at its edge and dropping what's burnt;
sparks born along the front rise, slow and fade, drawn as points that
add up their glow.

**Explode** (`explode.c`, the pieces in `explode_model.c`, unit-tested)
cuts a closing window's picture into rectangles, hexagons or glass shards
(rings round its middle cut by spokes, clipped to the window), each a
triangle fan of one mesh, flying out from the middle, spinning about an
axis of its own and coming toward the viewer in perspective, then
fading. Thickness is faked: a darker copy of each piece, pushed the way
it's tilted, shows as its edge. A window has one closing effect: the
first effect turned on to ask for it.

**Water Effect** (`water.c`, the waves in `water_model.c`, unit-tested)
is an output filter. A height field covers the output layout, a cell
every 4 layout units or more (at most 400 across, so llvmpipe keeps up),
stepped 60 times a second by the damped discrete wave equation; each
frame its slopes and light go to the GPU as a small RGBA texture, and the
shader shifts each pixel of the screen's picture by its slope (times
OffsetScale) and lightens or darkens it. Drops come from the Ripple
binding (Shift+Windows+click, which the click then doesn't reach), rain
(ToggleRain, Shift+F9, a drop every RainDelay ms), the pointer's wake
while Ctrl and the Windows key are held, and with TitleWave a line along
a window's title bar when its title changes. Still water with no rain
leaves the filter idle.

**Testing headless.** `tests/ui/run.py` runs a scenario declared with
`fx=True` on `xd-compositor-fx` with Mesa's llvmpipe, through EGL on no
device (`WLR_RENDERER_FORCE_SOFTWARE`, buffers from `/dev/udmabuf`), and
skips it where that can't draw; `XD_UI_RENDERER=gles2` uses the machine's
GPU instead.

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
- **Tabs** (Windows 11's; XP had none): a GtkNotebook over the folder
  view, its strip shown only with two or more. Each tab (`tab` in
  `main.c`) owns its XdFolderView, Back/Forward history and rename
  entry; the window (`explorer`) owns the menus, toolbar, address bar,
  Folders tree and status bar, which show the tab in front. A closed tab's
  struct stays until the window closes, so a mount or a network name still
  on its way finds it marked closed.
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
  in the temporary folders, shader, developer tools' and programs'
  caches), walking with `openat()` and never following links or crossing
  into another file system. Unused Flatpak runtimes go through libflatpak
  (`flatpakclean.c`; the system installation's through Flatpak's own
  polkit helper). The system files (`syscleanup.c`: the package cache, old
  journal files, crash dumps) are measured as the user and cleaned by
  `xd-cleanmgr-helper`, the one program of ours that runs as root: pkexec
  starts it for polkit's `org.xeondesktop.cleanmgr.clean`, it takes only
  the categories' names, and it runs the system's own tools (`pacman -Sc`,
  `apt-get clean`, `dnf clean packages`, `journalctl --vacuum-time`).
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
| Color Management | colord; monitors registered from `wlr-output-management`, their default profile in `desktop.ini` for the compositor |
| Date and Time | timedated; timesyncd (org.freedesktop.timesync1) and systemd for the time server and Update Now |
| Desktop Effects | `effects.ini` (see Desktop Effects), the compositor's support file |
| Display | `desktop.ini`, `monitors.ini`, `wlr-output-management`; Night Light's location from GeoClue |
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
  scenario that fails without the fix. Scenarios declared `fx` run on `xd-compositor-fx` with
  llvmpipe (see Desktop Effects).
- **CI**: GCC and Clang debug builds with the unit tests, optimized GCC and
  Clang builds, a GCC `-fanalyzer` build, the UI tests, and Debian, Arch
  and CachyOS packages built and installed in containers.

## Status

Every part described above exists and has been tested headless; TODO.md
lists what's untested on real hardware, the remaining Windows features, and
known gaps (among them a standalone taskbar for other wlroots compositors,
and Qt's file dialogs).
