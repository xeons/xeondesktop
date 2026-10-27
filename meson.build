project(
  'xeondesktop',
  'c',
  version: '0.5.0',
  license: 'MIT',
  meson_version: '>=1.3.0',
  default_options: [
    'c_std=c23',
    'warning_level=3',
    'werror=true',
    'buildtype=debug',
    'b_sanitize=address,undefined',
    'b_lundef=false',
    'b_pie=true',
  ],
)

cc = meson.get_compiler('c')

# Warnings beyond -Wall -Wextra -Wpedantic. Unsupported flags are dropped so
# the same file builds with GCC and Clang.
warnings = [
  '-Wconversion',
  '-Wsign-conversion',
  '-Wshadow',
  '-Wcast-qual',
  '-Wcast-align=strict',
  '-Wformat=2',
  '-Wformat-overflow=2',
  '-Wformat-truncation=2',
  '-Wformat-signedness',
  '-Wnull-dereference',
  '-Wdouble-promotion',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wmissing-declarations',
  '-Wold-style-definition',
  '-Wvla',
  '-Wundef',
  '-Wwrite-strings',
  '-Wpointer-arith',
  '-Wimplicit-fallthrough',
  '-Wswitch-enum',
  '-Wredundant-decls',
  '-Wnested-externs',
  '-Wduplicated-cond',
  '-Wduplicated-branches',
  '-Wlogical-op',
  '-Wjump-misses-init',
  '-Wstrict-overflow=2',
  '-Wstringop-overflow=4',
  '-Warray-bounds=2',
  '-Wshift-overflow=2',
  '-Walloca',
  '-Wbad-function-cast',
  '-Wflex-array-member-not-at-end',
  '-Wunused-macros',
  '-Wcomma',
  '-Wassign-enum',
  '-Wunreachable-code-aggressive',
  '-Wshorten-64-to-32',
  '-Wimplicit-int-conversion',
  '-Wfloat-conversion',
]

hardening = [
  '-fstack-protector-strong',
  '-fstack-clash-protection',
  '-fcf-protection=full',
  '-ftrivial-auto-var-init=zero',
  '-fno-strict-aliasing',
  '-fno-delete-null-pointer-checks',
  '-fstrict-flex-arrays=3',
]
if get_option('optimization') not in ['0', 'plain']
  hardening += ['-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=3']
endif

add_project_arguments(cc.get_supported_arguments(warnings + hardening), language: 'c')
add_project_link_arguments(
  cc.get_supported_link_arguments(['-Wl,-z,relro', '-Wl,-z,now', '-Wl,-z,noexecstack']),
  language: 'c',
)
if cc.get_id() == 'gcc' and get_option('analyzer')
  add_project_arguments('-fanalyzer', language: 'c')
endif

m_dep = cc.find_library('m')
# include_type 'system' keeps third-party headers out of our -Werror net.
cairo_dep = dependency('cairo', include_type: 'system')
pango_dep = dependency('pangocairo', include_type: 'system')
glib_dep = dependency('glib-2.0', include_type: 'system')

core_lib = static_library(
  'xdcore',
  'src/core/alloc.c',
  'src/core/str.c',
)
core_dep = declare_dependency(link_with: core_lib)

# libarchive reads .xdtheme zips (theme packages); without it they must be
# unpacked folders. It also makes Send To's Compressed (zipped) Folder.
archive_dep = dependency('libarchive', required: false, include_type: 'system')
theme_lib = static_library(
  'xdtheme',
  'src/theme/scheme.c',
  'src/theme/draw.c',
  'src/theme/theme.c',
  'src/theme/style_classic.c',
  'src/theme/style_modern.c',
  'src/theme/style_aero.c',
  'src/theme/style_whistler.c',
  'src/theme/luna.c',
  'src/theme/gtkcss.c',
  'src/theme/gtkpkg.c',
  'src/theme/trayicons.c',
  'src/theme/pkg.c',
  'src/theme/pkginfo.c',
  'src/theme/pkgfiles.c',
  'src/theme/style_pkg.c',
  'src/theme/switcher.c',
  'src/theme/controls.c',
  'src/theme/panes.c',
  'src/theme/scale.c',
  c_args: ['-DXD_HAVE_LIBARCHIVE=' + (archive_dep.found() ? '1' : '0'), '-D_GNU_SOURCE',
           '-DXD_THEME_DATADIR="' + get_option('prefix') / get_option('datadir') / 'xeondesktop"'],
  dependencies: [cairo_dep, pango_dep, glib_dep, m_dep, archive_dep],
)
theme_dep = declare_dependency(
  link_with: theme_lib,
  dependencies: [core_dep, cairo_dep, pango_dep, glib_dep, m_dep],
)

# The Qt palette file beside the GTK theme (src/settings/qtpalette.c).
json_glib_dep = dependency('json-glib-1.0', include_type: 'system')
settings_lib = static_library(
  'xdsettings',
  'src/settings/settings.c',
  'src/settings/state.c',
  'src/settings/monitors.c',
  'src/settings/gtktheme.c',
  'src/settings/qtpalette.c',
  'src/settings/sounds.c',
  'src/settings/inputmethod.c',
  'src/settings/nightlight.c',
  'src/settings/themepkg.c',
  'src/settings/themeset.c',
  # Desktop Effects: the table (one file per effect) and effects.ini.
  'src/settings/effects.c',
  'src/settings/effects/wobbly_windows.c',
  'src/settings/effects/magic_lamp.c',
  'src/settings/effects/burn.c',
  'src/settings/effects/explode.c',
  'src/settings/effects/water.c',
  'src/settings/effects/scale.c',
  'src/settings/effects/animations.c',
  'src/settings/effects/window_opacity.c',
  'src/settings/effects/ring_switcher.c',
  'src/settings/effects/enhanced_zoom.c',
  'src/settings/effects/annotate.c',
  'src/settings/effects/show_mouse.c',
  'src/settings/effects/general_options.c',
  'src/settings/effects/desktop_wall.c',
  'src/settings/effects/desktop_cube.c',
  'src/settings/effects/rotate_cube.c',
  'src/settings/effects/expo.c',
  'src/settings/effects/shift_switcher.c',
  dependencies: [cairo_dep, pango_dep, glib_dep, json_glib_dep, m_dep],
  c_args: ['-D_GNU_SOURCE'],
)
settings_dep = declare_dependency(link_with: settings_lib,
                                  dependencies: [theme_dep, glib_dep, json_glib_dep])

# -Dcompositor=false: the shell, libxdui and GTK programs without
# xd-compositor, for another wlroots compositor (sway, Hyprland, labwc...)
# on systems without wlroots 0.20. What only the compositor needs isn't
# looked for, and the disabler leaves out every target that uses it.
build_compositor = get_option('compositor')
if build_compositor
  # The system's, or our pinned copy (subprojects/wlroots.wrap) linked in
  # statically: the GLES2 and pixman renderers, DRM and libinput, Xwayland.
  wlroots_dep = dependency(
    'wlroots-0.20',
    include_type: 'system',
    default_options: ['default_library=static', 'examples=false', 'werror=false',
                      'renderers=gles2', 'backends=drm,libinput', 'allocators=gbm',
                      'session=enabled', 'xwayland=enabled', 'libliftoff=disabled',
                      'color-management=disabled', 'xcb-errors=disabled'],
  )
  xcb_dep = dependency('xcb', include_type: 'system')
  xcb_icccm_dep = dependency('xcb-icccm', include_type: 'system')
  wayland_server_dep = dependency('wayland-server', include_type: 'system')
  libdrm_dep = dependency('libdrm')
  libinput_dep = dependency('libinput', include_type: 'system')
  # Damage regions for window capture (capture.c).
  pixman_dep = dependency('pixman-1', include_type: 'system')
else
  wlroots_dep = disabler()
  xcb_dep = disabler()
  xcb_icccm_dep = disabler()
  wayland_server_dep = disabler()
  libdrm_dep = disabler()
  libinput_dep = disabler()
  pixman_dep = disabler()
endif
wayland_client_dep = dependency('wayland-client', include_type: 'system')
xkbcommon_dep = dependency('xkbcommon', include_type: 'system')

# ---- Wayland protocols --------------------------------------------------------
wayland_scanner = find_program(
  dependency('wayland-scanner', native: true).get_variable('wayland_scanner'),
  native: true,
)
wp_dir = dependency('wayland-protocols').get_variable('pkgdatadir')

layer_shell_xml = files('protocols/wlr-layer-shell-unstable-v1.xml')
x11_grab_xml = wp_dir / 'unstable/xwayland-keyboard-grab/xwayland-keyboard-grab-unstable-v1.xml'
client_protocol_xml = {
  'xdg-shell': wp_dir / 'stable/xdg-shell/xdg-shell.xml',
  'viewporter': wp_dir / 'stable/viewporter/viewporter.xml',
  'tablet-v2': wp_dir / 'stable/tablet/tablet-v2.xml',
  'fractional-scale-v1': wp_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
  'xdg-output-unstable-v1': wp_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
  'cursor-shape-v1': wp_dir / 'staging/cursor-shape/cursor-shape-v1.xml',
  'xdg-activation-v1': wp_dir / 'staging/xdg-activation/xdg-activation-v1.xml',
  'wlr-layer-shell-unstable-v1': layer_shell_xml,
  'wlr-foreign-toplevel-management-unstable-v1': files(
    'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  ),
  'wlr-virtual-pointer-unstable-v1': files('protocols/wlr-virtual-pointer-unstable-v1.xml'),
  'virtual-keyboard-unstable-v1': files('protocols/virtual-keyboard-unstable-v1.xml'),
  'input-method-unstable-v2': files('protocols/input-method-unstable-v2.xml'),
  'xd-window-arranger-v1': files('protocols/xd-window-arranger-v1.xml'),
  'xd-test-input-v1': files('protocols/xd-test-input-v1.xml'),
  'xd-window-rectangles-v1': files('protocols/xd-window-rectangles-v1.xml'),
  'xd-drag-relay-v1': files('protocols/xd-drag-relay-v1.xml'),
  'wlr-screencopy-unstable-v1': files('protocols/wlr-screencopy-unstable-v1.xml'),
  'ext-data-control-v1': wp_dir / 'staging/ext-data-control/ext-data-control-v1.xml',
  'pointer-gestures-unstable-v1':
    wp_dir / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml',
  'ext-idle-notify-v1': wp_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
  'ext-session-lock-v1': wp_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  'ext-foreign-toplevel-list-v1':
    wp_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  'ext-image-capture-source-v1':
    wp_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  'ext-image-copy-capture-v1':
    wp_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  'wlr-output-management-unstable-v1': files('protocols/wlr-output-management-unstable-v1.xml'),
  'wlr-output-power-management-unstable-v1': files(
    'protocols/wlr-output-power-management-unstable-v1.xml',
  ),
  'pointer-constraints-unstable-v1':
    wp_dir / 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml',
  'relative-pointer-unstable-v1':
    wp_dir / 'unstable/relative-pointer/relative-pointer-unstable-v1.xml',
  'keyboard-shortcuts-inhibit-unstable-v1':
    wp_dir / 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml',
  'xwayland-keyboard-grab-unstable-v1': x11_grab_xml,
  'security-context-v1': wp_dir / 'staging/security-context/security-context-v1.xml',
}

client_protocol_src = []
foreach name, xml : client_protocol_xml
  client_protocol_src += custom_target(
    name + '-client-header',
    input: xml,
    output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  client_protocol_src += custom_target(
    name + '-protocol-code',
    input: xml,
    output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# wlroots' output power header includes this generated server header.
server_protocol_headers_extra = [
  custom_target(
    'wlr-output-power-management-server-header',
    input: 'protocols/wlr-output-power-management-unstable-v1.xml',
    output: 'wlr-output-power-management-unstable-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  ),
]

# Our private protocol, server side (the compositor implements it).
arranger_xml = files('protocols/xd-window-arranger-v1.xml')
arranger_server_src = [
  custom_target(
    'xd-window-arranger-v1-server-header',
    input: arranger_xml,
    output: 'xd-window-arranger-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  ),
  custom_target(
    'xd-window-arranger-v1-server-code',
    input: arranger_xml,
    output: 'xd-window-arranger-v1-server-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  ),
  # VirtualBox's drag and drop relay (dragrelay.c).
  custom_target(
    'xd-drag-relay-v1-server-header',
    input: 'protocols/xd-drag-relay-v1.xml',
    output: 'xd-drag-relay-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  ),
  custom_target(
    'xd-drag-relay-v1-server-code',
    input: 'protocols/xd-drag-relay-v1.xml',
    output: 'xd-drag-relay-v1-server-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  ),
  # The Snipping Tool's Window Snip (rectangles.c).
  custom_target(
    'xd-window-rectangles-v1-server-header',
    input: 'protocols/xd-window-rectangles-v1.xml',
    output: 'xd-window-rectangles-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  ),
  custom_target(
    'xd-window-rectangles-v1-server-code',
    input: 'protocols/xd-window-rectangles-v1.xml',
    output: 'xd-window-rectangles-v1-server-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  ),
  # The UI tests' touchscreen, tablet and touchpad (testinput.c).
  custom_target(
    'xd-test-input-v1-server-header',
    input: 'protocols/xd-test-input-v1.xml',
    output: 'xd-test-input-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  ),
  custom_target(
    'xd-test-input-v1-server-code',
    input: 'protocols/xd-test-input-v1.xml',
    output: 'xd-test-input-v1-server-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  ),
  # Its attention event names a foreign-toplevel handle, whose interface
  # libwlroots doesn't export.
  custom_target(
    'wlr-foreign-toplevel-server-code',
    input: 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
    output: 'wlr-foreign-toplevel-management-unstable-v1-server-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  ),
]

# Xwayland's keyboard grab, which wlroots doesn't implement (constraints.c).
x11_grab_server_src = [
  custom_target(
    'xwayland-keyboard-grab-server-header',
    input: x11_grab_xml,
    output: 'xwayland-keyboard-grab-unstable-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  ),
  custom_target(
    'xwayland-keyboard-grab-server-code',
    input: x11_grab_xml,
    output: 'xwayland-keyboard-grab-unstable-v1-server-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  ),
]

# wlroots' layer-shell header includes the server header by this name.
server_protocol_headers = [
  custom_target(
    'wlr-layer-shell-server-header',
    input: layer_shell_xml,
    output: 'wlr-layer-shell-unstable-v1-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  ),
]

# Generated code isn't ours to fix; build it without the project warnings.
client_protocol_lib = static_library(
  'xdprotocols',
  client_protocol_src,
  dependencies: [wayland_client_dep],
  c_args: ['-w'],
)
client_protocol_dep = declare_dependency(
  link_with: client_protocol_lib,
  sources: client_protocol_src,
  dependencies: [wayland_client_dep],
)

compositor_src = files(
  'src/compositor/main.c',
  'src/compositor/output.c',
  'src/compositor/layers.c',
  'src/compositor/input.c',
  'src/compositor/touch.c',
  'src/compositor/tablet.c',
  'src/compositor/gestures.c',
  'src/compositor/testinput.c',
  'src/compositor/view.c',
  'src/compositor/frame.c',
  'src/compositor/animate.c',
  'src/compositor/transition.c',
  'src/compositor/outline.c',
  'src/compositor/switcher.c',
  'src/compositor/snap.c',
  'src/compositor/appicons.c',
  'src/compositor/arrange.c',
  'src/compositor/capture.c',
  'src/compositor/rectangles.c',
  'src/compositor/dragrelay.c',
  'src/compositor/protocols.c',
  'src/compositor/lock.c',
  'src/compositor/trust.c',
  'src/compositor/xwayland.c',
  'src/compositor/access.c',
  'src/compositor/ime.c',
  'src/compositor/present.c',
  'src/compositor/constraints.c',
  'src/compositor/desktops.c',
  'src/compositor/desktops_grid.c',
  'src/settings/soundplay.c',
)
compositor_src += x11_grab_server_src
# Gamma ramps: night light, and monitor calibration from ICC profiles'
# vcgt curves, read with Little CMS when it's there.
compositor_src += files('src/compositor/gamma.c', 'src/compositor/vcgt.c')
# Desktop Effects: effects.ini and window events for every effect, in both
# builds; the OpenGL effects (one file each) in xd-compositor-fx alone.
compositor_src += files(
  'src/compositor/effects/effects.c',
  'src/compositor/effects/mirror.c',
  'src/compositor/effects/scale.c',
  'src/compositor/effects/scale_model.c',
  'src/compositor/effects/animations.c',
  'src/compositor/effects/animations_model.c',
  'src/compositor/effects/opacity.c',
  'src/compositor/effects/ring.c',
  'src/compositor/effects/ring_model.c',
  'src/compositor/effects/zoom.c',
  'src/compositor/effects/annotate.c',
  'src/compositor/effects/showmouse.c',
  'src/compositor/effects/zoom_model.c',
  'src/compositor/effects/general.c',
  'src/compositor/effects/viewport_model.c',
  'src/compositor/effects/wall.c',
)
effects_fx_src = files(
  'src/compositor/effects/glfx.c',
  'src/compositor/effects/wobbly.c',
  'src/compositor/effects/wobbly_model.c',
  'src/compositor/effects/magic_lamp.c',
  'src/compositor/effects/magic_lamp_model.c',
  'src/compositor/effects/burn.c',
  'src/compositor/effects/burn_model.c',
  'src/compositor/effects/explode.c',
  'src/compositor/effects/explode_model.c',
  'src/compositor/effects/water.c',
  'src/compositor/effects/water_model.c',
  'src/compositor/effects/glfx_output.c',
  'src/compositor/effects/cube.c',
  'src/compositor/effects/expo.c',
  'src/compositor/effects/shift.c',
)
lcms2_dep = dependency('lcms2', required: false, include_type: 'system')
lcms2_args = lcms2_dep.found() ? ['-DXD_HAVE_LCMS2'] : []
gtk_dep = dependency('gtk+-3.0', include_type: 'system')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', include_type: 'system')
gio_unix_dep = dependency('gio-unix-2.0', include_type: 'system')

# Application icons from the icon theme, and which application a window
# belongs to: libxdui's, and the compositor's for title bars.
appicons_lib = static_library(
  'xdappicons',
  'src/ui/icons.c',
  'src/ui/image.c',
  'src/ui/appid.c',
  dependencies: [settings_dep, gtk_dep, gdk_pixbuf_dep, gio_unix_dep],
  c_args: ['-D_GNU_SOURCE'],
)
appicons_dep = declare_dependency(
  link_with: appicons_lib,
  dependencies: [settings_dep, gtk_dep, gdk_pixbuf_dep, gio_unix_dep],
)

# The sound scheme's sounds: the compositor's window events, the shell's,
# and every program with message boxes (gtkapp, libxdui).
canberra_dep = dependency('libcanberra', include_type: 'system')

compositor_deps = [
  canberra_dep,
  theme_dep,
  appicons_dep,
  settings_dep,
  wlroots_dep,
  xcb_dep,
  xcb_icccm_dep,
  wayland_server_dep,
  xkbcommon_dep,
  libdrm_dep,
  libinput_dep,
  pixman_dep,
  lcms2_dep,
]
executable(
  'xd-compositor',
  compositor_src,
  server_protocol_headers,
  server_protocol_headers_extra,
  arranger_server_src,
  dependencies: compositor_deps,
  c_args: ['-DWLR_USE_UNSTABLE', '-D_GNU_SOURCE'] + lcms2_args,
  install: true,
)
# The same compositor on SceneFX's scene graph: Aero's blurred glass and
# window shadows. GLES2 only, so xd-compositor hands over to it when it's
# installed, and it hands back when there's no GPU renderer.
# Always our pinned, patched copy (subprojects/scenefx.wrap), linked in
# statically; packages install with --skip-subprojects so none of it is
# installed.
scenefx_proj = subproject(
  'scenefx',
  required: get_option('aero_blur').require(build_compositor,
                                            error_message: 'needs -Dcompositor=true'),
  default_options: ['default_library=static', 'examples=false', 'werror=false'],
)
scenefx_dep = scenefx_proj.found() ? scenefx_proj.get_variable('scenefx') : dependency('', required: false)
if scenefx_dep.found()
  executable(
    'xd-compositor-fx',
    compositor_src,
    effects_fx_src,
    server_protocol_headers,
    server_protocol_headers_extra,
    arranger_server_src,
    # SceneFX first: its scene graph replaces wlroots' (same symbols), which
    # matters when wlroots is linked in statically too.
    dependencies: [scenefx_dep.as_system()] + compositor_deps,
    c_args: ['-DWLR_USE_UNSTABLE', '-D_GNU_SOURCE', '-DXD_SCENEFX'] + lcms2_args,
    install: true,
  )
endif

# The volume icon: PulseAudio's client library (PipeWire serves it too).
pulse_dep = dependency('libpulse-mainloop-glib', include_type: 'system')
# forkpty: in libc since glibc 2.34, in libutil before.
util_dep = cc.has_function('forkpty', prefix: '#include <pty.h>') ? declare_dependency() : cc.find_library('util')

# libxdui: Wayland client plumbing and dialog widgets for shell programs.
ui_lib = static_library(
  'xdui',
  'src/ui/shm.c',
  'src/ui/textedit.c',
  'src/ui/ui.c',
  'src/ui/widgets.c',
  'src/ui/display.c',
  dependencies: [theme_dep, settings_dep, appicons_dep, client_protocol_dep, gtk_dep,
                 gdk_pixbuf_dep, xkbcommon_dep],
  c_args: ['-D_GNU_SOURCE'],
)
ui_dep = declare_dependency(
  link_with: ui_lib,
  dependencies: [theme_dep, settings_dep, appicons_dep, client_protocol_dep, gtk_dep,
                 gdk_pixbuf_dep, xkbcommon_dep],
)

# Screen savers, shared by xd-lock and Display Properties' preview.
saver_lib = static_library(
  'xdsaver',
  'src/saver/savers.c',
  'src/saver/flower.c',
  'src/saver/maze.c',
  'src/saver/pipes.c',
  'src/saver/raster3d.c',
  'src/saver/text3d.c',
  dependencies: [theme_dep, settings_dep, m_dep],
  c_args: ['-D_GNU_SOURCE'],
)
saver_dep = declare_dependency(link_with: saver_lib, dependencies: [theme_dep, settings_dep])


apps_common = files('src/apps/common.c')

executable(
  'xd-endtask',
  'src/apps/endtask.c',
  dependencies: [ui_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)
executable(
  'xd-message',
  'src/apps/message.c',
  'src/settings/soundplay.c',
  dependencies: [ui_dep, canberra_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)
executable(
  'xd-run',
  'src/apps/run.c',
  'src/apps/runcmd.c',
  apps_common,
  dependencies: [ui_dep, settings_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-shutdown',
  'src/apps/shutdown.c',
  'src/settings/soundplay.c',
  'src/settings/login1.c',
  apps_common,
  dependencies: [ui_dep, canberra_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-desk',
  'src/apps/desk.c',
  'src/apps/desk_advanced.c',
  'src/apps/desk_color.c',
  'src/apps/desk_monitor.c',
  'src/apps/desk_nightlight.c',
  'src/apps/desk_saver.c',
  'src/settings/soundplay.c',
  'src/settings/themes.c',
  apps_common,
  dependencies: [ui_dep, saver_dep, canberra_dep, gio_unix_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-taskbar',
  'src/apps/taskbar.c',
  dependencies: [ui_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# Renaming files and launchers (GIO only): xd-files and the desktop icons.
rename_lib = static_library(
  'xdrename',
  'src/files/rename.c',
  dependencies: [gio_unix_dep],
)

# Thumbnails (freedesktop cache): the cache and the request queue, linked
# into xd-files; xd-thumbnailer decodes in a bubblewrap sandbox with a
# seccomp filter (libseccomp), bwrap being needed at run time.
thumb_lib = static_library(
  'xdthumb',
  'src/thumbnailer/thumbcache.c',
  'src/thumbnailer/queue.c',
  c_args: ['-DXD_VERSION="' + meson.project_version() + '"'],
  dependencies: [gdk_pixbuf_dep, gio_unix_dep],
)
seccomp_dep = dependency('libseccomp', include_type: 'system')
executable(
  'xd-thumbnailer',
  'src/thumbnailer/main.c',
  'src/thumbnailer/sandbox.c',
  c_args: ['-D_GNU_SOURCE'],
  link_with: thumb_lib,
  dependencies: [gdk_pixbuf_dep, gio_unix_dep, seccomp_dep],
  install: true,
)

# xd-files: the file manager (GTK3), with the folder view (libxdfolder).
# libarchive makes Send To's Compressed (zipped) Folder; without it the
# item isn't offered. libsecret remembers encrypted drives' passphrases;
# without it, they're asked for each time.
secret_dep = dependency('libsecret-1', required: false, include_type: 'system')
# Disk Cleanup's Unused Flatpak runtimes, and Add or Remove Programs.
flatpak_dep = dependency('flatpak', include_type: 'system')
folder_lib = static_library(
  'xdfolder',
  'src/files/chrome.c',
  'src/files/cleanup.c',
  'src/files/control.c',
  'src/files/drives.c',
  'src/files/encrypted.c',
  'src/files/extract.c',
  'src/files/favorites.c',
  'src/files/fileops.c',
  'src/files/flatpakclean.c',
  'src/files/folderview.c',
  'src/files/launcher.c',
  'src/files/luks.c',
  'src/files/luksdlg.c',
  'src/files/netdrives.c',
  'src/files/netui.c',
  'src/files/places.c',
  'src/files/properties.c',
  'src/files/search.c',
  'src/files/searchwalk.c',
  'src/files/sendto.c',
  'src/files/syscleanup.c',
  'src/files/taskpane.c',
  'src/files/udisks.c',
  'src/files/undo.c',
  'src/files/usershare.c',
  'src/files/versions.c',
  'src/files/versionsui.c',
  'src/cpl/wizard.c',
  c_args: ['-D_GNU_SOURCE', '-DXD_HAVE_LIBARCHIVE=' + (archive_dep.found() ? '1' : '0'),
           '-DXD_HAVE_LIBSECRET=' + (secret_dep.found() ? '1' : '0'),
           '-DXD_LIBEXECDIR="' + (get_option('prefix') / get_option('libexecdir')) + '"'],
  link_with: thumb_lib,
  dependencies: [core_dep, settings_dep, theme_dep, gtk_dep, gio_unix_dep, m_dep, archive_dep,
                 secret_dep, flatpak_dep],
)

# xd-control: opens Control Panel (control:/// in xd-files) or an applet.
executable(
  'xd-control',
  'src/control/main.c',
  'src/files/control.c',
  dependencies: [gio_unix_dep],
  install: true,
)
gtkapp_lib = static_library(
  'xdgtkapp',
  'src/gtkapp/gtkapp.c',
  'src/settings/soundplay.c',
  dependencies: [settings_dep, gtk_dep, canberra_dep],
)
install_data(
  'data/applications/org.xeondesktop.Files.desktop',
  'data/applications/org.xeondesktop.DiskCleanup.desktop',
  'data/applications/org.xeondesktop.TaskManager.desktop',
  'data/applications/org.xeondesktop.SnippingTool.desktop',
  'data/applications/org.xeondesktop.Minefield.desktop',
  'data/applications/org.xeondesktop.SystemProperties.desktop',
  'data/applications/org.xeondesktop.DateTime.desktop',
  'data/applications/org.xeondesktop.Display.desktop',
  'data/applications/org.xeondesktop.Taskbar.desktop',
  'data/applications/org.xeondesktop.Sound.desktop',
  'data/applications/org.xeondesktop.Power.desktop',
  'data/applications/org.xeondesktop.Privacy.desktop',
  'data/applications/org.xeondesktop.DesktopEffects.desktop',
  'data/applications/org.xeondesktop.ColorManagement.desktop',
  'data/applications/org.xeondesktop.RegionalOptions.desktop',
  'data/applications/org.xeondesktop.UserAccounts.desktop',
  'data/applications/org.xeondesktop.Keys.desktop',
  'data/applications/org.xeondesktop.FolderOptions.desktop',
  'data/applications/org.xeondesktop.Fonts.desktop',
  'data/applications/org.xeondesktop.Mouse.desktop',
  'data/applications/org.xeondesktop.Keyboard.desktop',
  'data/applications/org.xeondesktop.Accessibility.desktop',
  'data/applications/org.xeondesktop.ScheduledTasks.desktop',
  'data/applications/org.xeondesktop.Bluetooth.desktop',
  'data/applications/org.xeondesktop.GameControllers.desktop',
  'data/applications/org.xeondesktop.NetworkConnections.desktop',
  'data/applications/org.xeondesktop.AdministrativeTools.desktop',
  'data/applications/org.xeondesktop.Services.desktop',
  'data/applications/org.xeondesktop.EventViewer.desktop',
  'data/applications/org.xeondesktop.DiskManagement.desktop',
  'data/applications/org.xeondesktop.AddRemovePrograms.desktop',
  'data/applications/org.xeondesktop.Flatpak.desktop',
  'data/applications/org.xeondesktop.Printers.desktop',
  'data/applications/org.xeondesktop.Scanners.desktop',
  'data/applications/org.xeondesktop.Search.desktop',
  'data/applications/org.xeondesktop.Help.desktop',
  'data/applications/org.xeondesktop.Defaults.desktop',
  'data/applications/org.xeondesktop.AutomaticUpdates.desktop',
  install_dir: get_option('datadir') / 'applications',
)

# xd-taskmgr: the Task Manager (GTK3), with process data from libproc2.
proc2_dep = dependency('libproc2', include_type: 'system')
# Older libproc2 (4.0.4, Ubuntu 26.04) takes the info in PIDS_VAL and
# STAT_VAL too.
taskmgr_args = ['-D_GNU_SOURCE']
if not cc.compiles('''#include <libproc2/pids.h>
int f(struct pids_stack *s) { return PIDS_VAL(PIDS_ID_PID, s_int, s); }''',
                   dependencies: proc2_dep, name: 'libproc2 PIDS_VAL without the info')
  taskmgr_args += '-DXD_PROC2_VAL_TAKES_INFO'
endif
# The Handles column's item, since libproc2 4.0.5.
if cc.has_header_symbol('libproc2/pids.h', 'PIDS_OPEN_FILES', dependencies: proc2_dep)
  taskmgr_args += '-DXD_PROC2_HAS_OPEN_FILES'
endif
executable(
  'xd-taskmgr',
  'src/taskmgr/main.c',
  'src/taskmgr/sysinfo.c',
  'src/taskmgr/graph.c',
  'src/taskmgr/tasks.c',
  'src/taskmgr/trayicon.c',
  'src/taskmgr/netstat.c',
  'src/taskmgr/network.c',
  'src/taskmgr/users.c',
  'src/taskmgr/widgets.c',
  'src/settings/login1.c',
  c_args: taskmgr_args,
  link_with: gtkapp_lib,
  dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep, proc2_dep, client_protocol_dep, m_dep],
  install: true,
)
executable(
  'xd-files',
  'src/files/main.c',
  link_with: [folder_lib, gtkapp_lib, rename_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, m_dep],
  install: true,
)

# The Snipping Tool: its overlay is a layer-shell surface, which GTK 3
# makes through gtk-layer-shell; screen recordings are encoded with
# GStreamer.
gtk_layer_shell_dep = dependency('gtk-layer-shell-0', include_type: 'system')
gstreamer_dep = dependency('gstreamer-1.0', include_type: 'system')
gstreamer_app_dep = dependency('gstreamer-app-1.0', include_type: 'system')
executable(
  'xd-snip',
  'src/snip/main.c',
  'src/snip/capture.c',
  'src/snip/overlay.c',
  'src/snip/editor.c',
  'src/snip/snipping.c',
  'src/snip/record.c',
  'src/snip/recording.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, gtk_layer_shell_dep, gdk_pixbuf_dep,
                 gio_unix_dep, client_protocol_dep, m_dep, gstreamer_dep, gstreamer_app_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# VirtualBox drag and drop for the guest (VBoxClient's is X11-only),
# started by the compositor in a VirtualBox guest.
executable(
  'xd-vboxdnd',
  'src/vboxdnd/main.c',
  'src/vboxdnd/dnd.c',
  'src/vboxdnd/hgcm.c',
  'src/vboxdnd/fakehost.c',
  dependencies: [glib_dep, client_protocol_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# Minefield, Windows XP's mine-clearing game.
executable(
  'xd-mines',
  'src/mines/main.c',
  'src/mines/minefield.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, m_dep],
  install: true,
)

executable(
  'xd-sysdm',
  'src/sysdm/main.c',
  'src/sysdm/hardware.c',
  'src/sysdm/advanced.c',
  'src/users/accounts.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, dependency('libudev')],
  c_args: ['-D_GNU_SOURCE', '-DXD_VERSION="' + meson.project_version() + '"'],
  install: true,
)

executable(
  'xd-polkit',
  'src/polkit/agent.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, dependency('polkit-agent-1')],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-security',
  'src/security/main.c',
  'src/security/passwd.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, util_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-timedate',
  'src/timedate/main.c',
  'src/timedate/ntp.c',
  'src/timedate/zone.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, m_dep,
                 dependency('json-glib-1.0', include_type: 'system')],
  install: true,
)

xkbregistry_dep = dependency('xkbregistry', include_type: 'system')
executable(
  'xd-intl',
  'src/intl/main.c',
  'src/intl/customize.c',
  'src/intl/intl.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, xkbregistry_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

crypt_dep = dependency('libxcrypt', required: false, include_type: 'system')
if not crypt_dep.found()
  crypt_dep = cc.find_library('crypt')
endif
executable(
  'xd-users',
  'src/users/main.c',
  'src/users/users.c',
  'src/users/accounts.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, crypt_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# Passwords and Keys (User Accounts): the Secret Service through
# libsecret, SSH keys through OpenSSH's tools, GnuPG through gpgme.
libsecret_dep = dependency('libsecret-1', include_type: 'system')
gpgme_dep = dependency('gpgme', include_type: 'system')
executable(
  'xd-keys',
  'src/keys/main.c',
  'src/keys/passwords.c',
  'src/keys/sshpage.c',
  'src/keys/gpgpage.c',
  'src/keys/keyslib.c',
  'src/keys/sshkeys.c',
  'src/keys/gpgkeys.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, libsecret_dep, gpgme_dep],
  install: true,
)

executable(
  'xd-sound',
  'src/sound/main.c',
  'src/sound/scheme.c',
  'src/settings/soundplay.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, pulse_dep, canberra_dep, m_dep],
  install: true,
)

# NUT's client library: Power Options' UPS tab and the shell ask upsd.
upsclient_dep = dependency('libupsclient', include_type: 'system')
executable(
  'xd-power',
  'src/power/main.c',
  'src/power/schemes.c',
  'src/power/upsstate.c',
  'src/power/upssource.c',
  'src/power/upstab.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, upsclient_dep],
  install: true,
)

# Privacy: recent documents, old files, location, portal permissions.
executable(
  'xd-privacy',
  'src/privacy/main.c',
  'src/privacy/permissions.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep],
  install: true,
)

# Desktop Effects: the compositor's Compiz-style effects (effects.ini),
# every page built from the table in src/settings/effects.h.
executable(
  'xd-effects',
  'src/effects/main.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)

# Color Management (colord's devices and profiles; the monitors from
# wlr-output-management).
executable(
  'xd-color',
  'src/color/main.c',
  'src/color/colorlib.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep, client_protocol_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# Folder Options and Fonts (Control Panel).
executable(
  'xd-folders',
  'src/folders/main.c',
  'src/folders/filetypes.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)
fontconfig_dep = dependency('fontconfig', include_type: 'system')
# The viewer draws a font file alone through Pango's fontconfig font map.
pangoft2_dep = dependency('pangoft2', include_type: 'system')
executable(
  'xd-fonts',
  'src/fonts/main.c',
  'src/fonts/viewer.c',
  'src/fonts/fontlist.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, fontconfig_dep, pangoft2_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# Mouse, Keyboard and Accessibility Options: [Mouse], [Keyboard] and
# [Accessibility] in desktop.ini, which the compositor applies.
udev_dep = dependency('libudev')
executable(
  'xd-mouse',
  'src/mouse/main.c',
  'src/mouse/cursors.c',
  'src/cpl/inputdevs.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, udev_dep, dependency('xcursor')],
  install: true,
)
executable(
  'xd-keyboard',
  'src/keyboard/main.c',
  'src/keyboard/layouts.c',
  'src/cpl/inputdevs.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, udev_dep, dependency('xkbregistry')],
  install: true,
)
executable(
  'xd-access',
  'src/access/main.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)

# The Start menus' Search, Help and Set Program Access and Defaults.
executable(
  'xd-search',
  'src/startmenu/search.c',
  'src/startmenu/searchlib.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)
executable(
  'xd-help',
  'src/startmenu/help.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)
executable(
  'xd-defaults',
  'src/startmenu/defaults.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)

# Scheduled Tasks (Control Panel): systemd user timers, over D-Bus.
executable(
  'xd-schedtasks',
  'src/schedtasks/main.c',
  'src/schedtasks/editor.c',
  'src/schedtasks/wizard.c',
  'src/schedtasks/props.c',
  'src/schedtasks/units.c',
  'src/schedtasks/task.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep],
  install: true,
)

# System Configuration Utility (msconfig): autostart entries and the
# systemd user services that start with the session.
executable(
  'xd-msconfig',
  'src/startup/main.c',
  'src/startup/autostart.c',
  'src/startup/services.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)
install_data('data/applications/org.xeondesktop.SystemConfiguration.desktop',
             install_dir: get_option('datadir') / 'applications')
install_man('man/xd-msconfig.1')

# Bluetooth Devices (BlueZ over D-Bus) and Game Controllers (udev and
# evdev, read-only).
executable(
  'xd-bluetooth',
  'src/bluetooth/main.c',
  'src/bluetooth/agent.c',
  'src/bluetooth/devprops.c',
  'src/bluetooth/wizard.c',
  'src/bluetooth/btinfo.c',
  c_args: ['-D_GNU_SOURCE'],
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, udev_dep],
  install: true,
)
executable(
  'xd-joy',
  'src/joy/main.c',
  'src/joy/joyinfo.c',
  c_args: ['-D_GNU_SOURCE'],
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep, udev_dep, dependency('libevdev')],
  install: true,
)

# Network Connections: NetworkManager through libnm.
libnm_dep = dependency('libnm', include_type: 'system')
executable(
  'xd-netconn',
  'src/netconn/main.c',
  'src/netconn/netconn.c',
  'src/netconn/status.c',
  'src/netconn/props.c',
  'src/netconn/wizard.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, libnm_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# Administrative Tools: Services (systemd), Event Viewer (journald), Disk
# Management (UDisks2) and the folder that lists them.
admin_lib = static_library('xdadmin', 'src/admin/admin.c', dependencies: [glib_dep])
foreach tool : ['services', 'diskmgmt', 'admintools']
  executable(
    'xd-' + tool,
    'src/admin/' + tool + '.c',
    # Disk Management's passphrase boxes, shared with My Computer.
    tool == 'diskmgmt' ? files('src/files/luks.c', 'src/files/luksdlg.c') : [],
    link_with: [gtkapp_lib, admin_lib],
    dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep],
    install: true,
  )
endforeach
executable(
  'xd-eventvwr',
  'src/admin/eventvwr.c',
  link_with: [gtkapp_lib, admin_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, dependency('libsystemd', include_type: 'system')],
  install: true,
)

# Add or Remove Programs: PackageKit and Flatpak.
packagekit_dep = dependency('packagekit-glib2', include_type: 'system')
executable(
  'xd-appwiz',
  'src/appwiz/main.c',
  'src/appwiz/add.c',
  'src/appwiz/ops.c',
  'src/appwiz/programs.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, packagekit_dep, flatpak_dep],
  install: true,
)
# Flatpak: app permissions (the user's overrides) and remotes.
executable(
  'xd-flatpak',
  'src/flatpak/main.c',
  'src/flatpak/appperms.c',
  'src/flatpak/perms.c',
  'src/flatpak/remotes.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, flatpak_dep],
  install: true,
)

# Automatic Updates: PackageKit and Flatpak updates, fwupd's firmware (over
# D-Bus; firmware is downloaded with libsoup), and the shell's check.
executable(
  'xd-wuaucpl',
  'src/wuaucpl/main.c',
  'src/wuaucpl/software.c',
  'src/wuaucpl/firmware.c',
  'src/wuaucpl/updlib.c',
  'src/wuaucpl/state.c',
  'src/appwiz/programs.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep, packagekit_dep, flatpak_dep,
                 dependency('libsoup-3.0', include_type: 'system')],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

# Printers and Faxes (CUPS) and Scanners and Cameras (SANE, libgphoto2).
cups_dep = dependency('cups', include_type: 'system')
sane_dep = dependency('sane-backends', include_type: 'system')
gphoto2_dep = dependency('libgphoto2', include_type: 'system')
executable(
  'xd-printers',
  'src/printers/main.c',
  'src/printers/actions.c',
  'src/printers/addwizard.c',
  'src/printers/auth.c',
  'src/printers/cupsops.c',
  'src/printers/printlib.c',
  'src/printers/props.c',
  'src/printers/queue.c',
  'src/cpl/wizard.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, cups_dep],
  c_args: ['-D_GNU_SOURCE',
           '-DXD_CUPS_DATADIR="' + cups_dep.get_variable(pkgconfig: 'cups_datadir',
                                                          default_value: '/usr/share/cups') + '"'],
  install: true,
)
# A stand-in for cupsd, for the UI tests (cupsd itself needs root).
executable('xd-cupsmock', 'tests/cupsmock.c', dependencies: [glib_dep, cups_dep])
executable(
  'xd-scanners',
  'src/scanners/main.c',
  'src/scanners/devices.c',
  'src/scanners/imgwizard.c',
  'src/scanners/scanlib.c',
  'src/cpl/wizard.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, sane_dep, gphoto2_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-wifi-key',
  'src/gtkapp/wifikey.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)

executable(
  'xd-choose-file',
  'src/gtkapp/choose.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
  install: true,
)

# Our Open and Save As dialogs for every program asking xdg-desktop-portal
# for a file. Its D-Bus interfaces are xdg-desktop-portal's own files
# (xdg-desktop-portal's pkg-config gives where), built in as a resource.
gnome = import('gnome')
portal_dep = dependency('xdg-desktop-portal', include_type: 'system')
portal_interfaces = portal_dep.get_variable(pkgconfig: 'interfaces_dir')
portal_resources = gnome.compile_resources(
  'xd-portal-resources',
  'data/xdg-desktop-portal/xeondesktop.gresource.xml',
  source_dir: portal_interfaces,
  c_name: 'xd_portal',
  extra_args: ['--internal'],
)
# glib-compile-resources' code isn't ours: built apart, without our
# warnings.
portal_resources_lib = static_library(
  'xdportalresources',
  portal_resources,
  dependencies: [gio_unix_dep],
  override_options: ['warning_level=0', 'werror=false'],
  c_args: ['-w'],
)
executable(
  'xdg-desktop-portal-xeondesktop',
  'src/portal/main.c',
  'src/portal/chooser.c',
  'src/portal/chooserlib.c',
  'src/shell/recent.c',
  c_args: ['-D_GNU_SOURCE'],
  link_with: [folder_lib, gtkapp_lib, rename_lib, portal_resources_lib],
  dependencies: [core_dep, settings_dep, theme_dep, gtk_dep, gio_unix_dep, m_dep],
  install: true,
  install_dir: get_option('libexecdir'),
)
portal_libexec = {'LIBEXECDIR': get_option('prefix') / get_option('libexecdir')}
install_data('data/xdg-desktop-portal/portals/xeondesktop.portal',
             install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')
configure_file(
  input: 'data/dbus-1/services/org.freedesktop.impl.portal.desktop.xeondesktop.service.in',
  output: 'org.freedesktop.impl.portal.desktop.xeondesktop.service',
  configuration: portal_libexec,
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

executable(
  'xd-shell',
  'src/shell/main.c',
  'src/shell/idle.c',
  'src/shell/taskbar.c',
  'src/shell/toplevels.c',
  'src/shell/apps.c',
  'src/shell/gpu.c',
  'src/files/control.c',
  'src/shell/menu.c',
  'src/shell/startmenu.c',
  'src/shell/deskicons.c',
  'src/shell/desktop.c',
  'src/shell/quicklaunch.c',
  'src/shell/start7.c',
  'src/shell/tooltip.c',
  'src/shell/balloon.c',
  'src/shell/notices.c',
  'src/shell/notifyd.c',
  'src/shell/notifyfly.c',
  'src/shell/tray.c',
  'src/shell/dnd.c',
  'src/shell/sys_battery.c',
  'src/shell/ups.c',
  'src/power/upsstate.c',
  'src/power/upssource.c',
  'src/shell/sys_access.c',
  'src/shell/sys_bluetooth.c',
  'src/shell/sys_network.c',
  'src/shell/sys_volume.c',
  'src/shell/clockfly.c',
  'src/shell/calendar.c',
  'src/shell/taskmenu.c',
  'src/shell/recent.c',
  'src/shell/superbar.c',
  'src/shell/thumbs.c',
  'src/shell/mpris.c',
  'src/shell/launcher.c',
  'src/shell/jumplist.c',
  'src/shell/sleep.c',
  'src/shell/buttons.c',
  'src/shell/account.c',
  'src/shell/sounds.c',
  'src/shell/shellbus.c',
  'src/shell/identify.c',
  'src/shell/session.c',
  'src/shell/primary.c',
  'src/shell/langbar.c',
  'src/shell/pager.c',
  'src/shell/sys_updates.c',
  'src/wuaucpl/state.c',
  'src/shell/housekeeping.c',
  'src/files/cleanup.c',
  'src/settings/soundplay.c',
  'src/settings/themes.c',
  link_with: rename_lib,
  dependencies: [ui_dep, saver_dep, glib_dep, gio_unix_dep, pulse_dep, canberra_dep, udev_dep,
                 xkbregistry_dep, json_glib_dep, upsclient_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-lock',
  'src/saver/lock.c',
  'src/settings/login1.c',
  dependencies: [
    ui_dep,
    settings_dep,
    saver_dep,
    dependency('pam'),
    xkbcommon_dep,
    dependency('threads'),
  ],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)
install_data('data/pam.d/xeondesktop', install_dir: get_option('sysconfdir') / 'pam.d')
# ananicy-cpp (CachyOS and others): the compositor's priority, as for other
# desktops' compositors.
install_data('data/ananicy/xeondesktop.rules',
             install_dir: get_option('sysconfdir') / 'ananicy.d' / 'xeondesktop')

executable('xd-themeview', 'tools/xd-themeview.c', dependencies: [theme_dep],
           c_args: ['-DXD_SOURCE_ROOT="' + meson.project_source_root() + '"'])
# Draws each part with a built-in style and with a theme package, and
# scores their difference (matching our styles to the real ones).
executable('xd-theme-compare', 'tools/xd-theme-compare.c', dependencies: [theme_dep, m_dep],
           c_args: ['-DXD_SOURCE_ROOT="' + meson.project_source_root() + '"'])
# Scales a PNG with every theme image filter, to compare them by eye.
executable('xd-scale-demo', 'tools/xd-scale-demo.c', dependencies: [theme_dep])

# The wallpapers, where Display Properties' Background list finds them:
# Energy painted by tools/xd-wallpapers (run by hand; the picture is
# committed), the others pictures of the project's own.
executable('xd-wallpapers', 'tools/xd-wallpapers.c', dependencies: [gdk_pixbuf_dep, m_dep])
install_data(
  'data/wallpapers/Watercolor Blue.jpg',
  'data/wallpapers/Storm Hill.jpg',
  'data/wallpapers/Olive Forest.jpg',
  'data/wallpapers/Silver Factory.jpg',
  'data/wallpapers/Energy.jpg',
  install_dir: get_option('datadir') / 'wallpapers' / 'xeondesktop',
)

# Drives a session with --allow-virtual-input, for automated UI tests.
executable(
  'xd-inject',
  'tools/xd-inject.c',
  dependencies: [core_dep, client_protocol_dep, xkbcommon_dep],
  c_args: ['-D_GNU_SOURCE'],
)

# A stand-in input method (input-method-v2), for the UI tests.
executable(
  'xd-test-ime',
  'tools/xd-test-ime.c',
  dependencies: [client_protocol_dep],
  c_args: ['-D_GNU_SOURCE'],
)

# A window that logs the input reaching it (touch, pens, gestures), for
# the UI tests.
executable(
  'xd-test-input',
  'tools/xd-test-input.c',
  dependencies: [client_protocol_dep],
  c_args: ['-D_GNU_SOURCE'],
)
# A program capturing the mouse and keyboard (pointer lock and
# confinement, relative motion, taking the shortcuts), for the UI tests;
# and an X11 one grabbing them through Xwayland.
executable(
  'xd-test-grab',
  'tools/xd-test-grab.c',
  dependencies: [client_protocol_dep],
  c_args: ['-D_GNU_SOURCE'],
)
executable(
  'xd-test-x11grab',
  'tools/xd-test-x11grab.c',
  dependencies: [xcb_dep],
)

# Lists the globals a client, or a sandboxed one, gets, for the UI tests.
executable(
  'xd-globals',
  'tools/xd-globals.c',
  c_args: ['-D_GNU_SOURCE'],
  dependencies: [client_protocol_dep],
)

# Reads a session's clipboard (ext-data-control), for the UI tests.
executable(
  'xd-paste',
  'tools/xd-paste.c',
  dependencies: [client_protocol_dep],
)

# Lists a session's windows (title, app id, state), for the UI tests.
executable(
  'xd-windows',
  'tools/xd-windows.c',
  dependencies: [core_dep, client_protocol_dep],
)

test_exe = executable(
  'xd-tests',
  'tests/test_main.c',
  'src/shell/launcher.c',
  'src/shell/apps.c',
  'src/shell/gpu.c',
  'src/shell/jumplist.c',
  'src/shell/calendar.c',
  'src/shell/notices.c',
  'src/files/control.c',
  'src/ui/textedit.c',
  'src/apps/runcmd.c',
  'src/security/passwd.c',
  'src/intl/intl.c',
  'src/users/users.c',
  'src/users/accounts.c',
  'src/folders/filetypes.c',
  'src/fonts/fontlist.c',
  'src/power/schemes.c',
  'src/power/upsstate.c',
  'src/keyboard/layouts.c',
  'src/mouse/cursors.c',
  'src/settings/themes.c',
  'src/schedtasks/task.c',
  'src/startup/autostart.c',
  'src/startup/services.c',
  'src/bluetooth/btinfo.c',
  'src/joy/joyinfo.c',
  'src/netconn/netconn.c',
  'src/admin/admin.c',
  'src/appwiz/programs.c',
  'src/wuaucpl/updlib.c',
  'src/wuaucpl/state.c',
  'src/flatpak/perms.c',
  'src/printers/printlib.c',
  'src/printers/cupsops.c',
  'src/scanners/scanlib.c',
  'src/startmenu/searchlib.c',
  'src/files/favorites.c',
  'src/files/launcher.c',
  'src/files/cleanup.c',
  'src/files/syscleanup.c',
  'src/files/luks.c',
  'src/files/netdrives.c',
  'src/files/usershare.c',
  'src/taskmgr/netstat.c',
  'src/timedate/ntp.c',
  'src/timedate/zone.c',
  'src/shell/recent.c',
  'src/privacy/permissions.c',
  'src/compositor/vcgt.c',
  'src/compositor/effects/wobbly_model.c',
  'src/compositor/effects/scale_model.c',
  'src/compositor/effects/animations_model.c',
  'src/compositor/effects/ring_model.c',
  'src/compositor/effects/zoom_model.c',
  'src/compositor/desktops_grid.c',
  'src/compositor/effects/viewport_model.c',
  'src/color/colorlib.c',
  'src/keys/keyslib.c',
  'src/keys/sshkeys.c',
  'src/keys/gpgkeys.c',
  'src/files/extract.c',
  'src/files/search.c',
  'src/files/searchwalk.c',
  'src/snip/snipping.c',
  'src/snip/recording.c',
  'src/mines/minefield.c',
  'src/vboxdnd/dnd.c',
  'src/vboxdnd/hgcm.c',
  'src/vboxdnd/fakehost.c',
  'src/files/versions.c',
  'src/portal/chooserlib.c',
  'src/compositor/effects/magic_lamp_model.c',
  'src/compositor/effects/burn_model.c',
  'src/compositor/effects/explode_model.c',
  'src/compositor/effects/water_model.c',
  link_with: [thumb_lib, saver_lib],
  dependencies: [theme_dep, settings_dep, glib_dep, gio_unix_dep, gtk_dep, util_dep, crypt_dep,
                 fontconfig_dep, packagekit_dep, cups_dep, gpgme_dep,
                 sane_dep.partial_dependency(compile_args: true), lcms2_dep, archive_dep],
  c_args: ['-DXD_SOURCE_ROOT="' + meson.project_source_root() + '"', '-D_GNU_SOURCE',
           '-DXD_VERSION="' + meson.project_version() + '"',
           '-DXD_HAVE_LIBARCHIVE=' + (archive_dep.found() ? '1' : '0')] + lcms2_args,
)
test(
  'unit',
  test_exe,
  env: [
    'ASAN_OPTIONS=detect_leaks=1',
    'LSAN_OPTIONS=suppressions=' + meson.project_source_root() / 'tests/lsan.supp' + ':print_suppressions=0',
    'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1',
    # The source tree's data (uxtheme.ini, schemes): a build directory
    # outside the source tree (makepkg's) has no ../data beside it.
    'XD_DATA_DIR=' + meson.project_source_root() / 'data',
  ],
)

# UI tests (tests/ui/run.py): each scenario drives a headless session of its
# own. They test what's built, so build first, and they're left out of a
# plain `meson test`:
#   ninja -C build && meson test -C build --setup ui --suite ui
# A failed scenario's screenshots and log go to meson-logs/ui.
python3 = find_program('python3', required: false)
if python3.found()
  # xd-theme-import's reading of XP's shellstyle.dll stylesheets.
  test('theme-import', python3, args: [files('tests/test_theme_import.py')])
  ui_runner = files('tests/ui/run.py')
  ui_scenarios = run_command(python3, ui_runner, '--names', check: true).stdout().split()
  foreach name : ui_scenarios
    test(
      name,
      python3,
      args: [ui_runner, '--build-dir', meson.project_build_root(),
             '--keep', meson.project_build_root() / 'meson-logs' / 'ui', name],
      suite: 'ui',
      timeout: 240,
    )
  endforeach
endif
add_test_setup('default', is_default: true, exclude_suites: ['ui'])
add_test_setup('ui')

install_subdir('data/schemes', install_dir: get_option('datadir') / 'xeondesktop')
install_subdir('data/maps', install_dir: get_option('datadir') / 'xeondesktop')

systemd_pc = dependency('systemd', required: false)
systemd_user_unit_dir = systemd_pc.found() \
  ? systemd_pc.get_variable(pkgconfig: 'systemd_user_unit_dir',
                            pkgconfig_define: ['prefix', get_option('prefix')]) \
  : get_option('prefix') / 'lib' / 'systemd' / 'user'
if build_compositor
  # The session, for display managers.
  install_data('data/xeondesktop-session', install_dir: get_option('bindir'),
               install_mode: 'rwxr-xr-x')
  # Started by the compositor, stopped by xeondesktop-session: binds
  # graphical-session.target to the session.
  install_data('data/systemd/user/xeondesktop-session.target',
               install_dir: systemd_user_unit_dir)
  install_data(
    'data/wayland-sessions/xeondesktop.desktop',
    install_dir: get_option('datadir') / 'wayland-sessions',
  )
  install_data(
    'data/xdg-desktop-portal/xeondesktop-portals.conf',
    install_dir: get_option('datadir') / 'xdg-desktop-portal',
  )
endif
# The taskbar alone under another compositor's session, for those who
# enable it (systemctl --user enable xeondesktop-taskbar).
configure_file(
  input: 'data/systemd/user/xeondesktop-taskbar.service.in',
  output: 'xeondesktop-taskbar.service',
  configuration: {'BINDIR': get_option('prefix') / get_option('bindir')},
  install_dir: systemd_user_unit_dir,
  install_tag: 'runtime',
)
# The portal backend, as D-Bus activation starts it (as GTK's is).
configure_file(
  input: 'data/systemd/user/xdg-desktop-portal-xeondesktop.service.in',
  output: 'xdg-desktop-portal-xeondesktop.service',
  configuration: portal_libexec,
  install_dir: systemd_user_unit_dir,
  install_tag: 'runtime',
)
# Theme packages: Windows visual styles converted with xd-theme-import
# (python-pefile, PyGObject's GdkPixbuf, PyCairo); uxtheme's names for it and the
# theme engine. docs/THEMES.md.
install_data('tools/xd-theme-import', install_dir: get_option('bindir'),
             install_mode: 'rwxr-xr-x')
install_data('data/theme/uxtheme.ini',
             install_dir: get_option('datadir') / 'xeondesktop' / 'theme')
# Disk Cleanup's system files (the package cache, old system logs, crash
# dumps), cleaned as root through pkexec and the polkit action below.
executable(
  'xd-cleanmgr-helper',
  'src/files/cleanmgr_helper.c',
  'src/files/syscleanup.c',
  'src/files/cleanup.c',
  dependencies: [glib_dep, gio_unix_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
  install_dir: get_option('libexecdir'),
)
configure_file(
  input: 'data/polkit/org.xeondesktop.cleanmgr.policy.in',
  output: 'org.xeondesktop.cleanmgr.policy',
  configuration: {'LIBEXECDIR': get_option('prefix') / get_option('libexecdir')},
  install_dir: get_option('datadir') / 'polkit-1' / 'actions',
)
# VirtualBox guests: its shared clipboard as a Wayland client.
install_data('data/xd-vboxclient', install_dir: get_option('libexecdir'),
             install_mode: 'rwxr-xr-x')
configure_file(
  input: 'data/systemd/user/app-vboxclient@autostart.service.d/xeondesktop.conf',
  output: 'xeondesktop-vboxclient.conf',
  configuration: {'LIBEXECDIR': get_option('prefix') / get_option('libexecdir')},
  install_dir: systemd_user_unit_dir / 'app-vboxclient@autostart.service.d',
  install_tag: 'runtime',
)

# Manual pages (roff, in man/), one per program installed in bindir.
install_man(
  'man/xd-access.1', 'man/xd-admintools.1', 'man/xd-appwiz.1', 'man/xd-bluetooth.1',
  'man/xd-choose-file.1', 'man/xd-color.1', 'man/xd-control.1', 'man/xd-defaults.1', 'man/xd-desk.1',
  'man/xd-diskmgmt.1', 'man/xd-effects.1', 'man/xd-endtask.1', 'man/xd-eventvwr.1',
  'man/xd-files.1',
  'man/xd-flatpak.1',
  'man/xd-folders.1', 'man/xd-fonts.1', 'man/xd-help.1', 'man/xd-intl.1', 'man/xd-joy.1',
  'man/xd-keys.1',
  'man/xd-keyboard.1', 'man/xd-lock.1', 'man/xd-message.1', 'man/xd-mines.1', 'man/xd-mouse.1',
  'man/xd-netconn.1', 'man/xd-polkit.1', 'man/xd-power.1', 'man/xd-printers.1', 'man/xd-privacy.1',
  'man/xd-run.1', 'man/xd-scanners.1', 'man/xd-schedtasks.1', 'man/xd-search.1',
  'man/xd-security.1', 'man/xd-services.1', 'man/xd-shell.1', 'man/xd-shutdown.1', 'man/xd-snip.1',
  'man/xd-sound.1', 'man/xd-sysdm.1', 'man/xd-taskbar.1', 'man/xd-taskmgr.1',
  'man/xd-thumbnailer.1', 'man/xd-timedate.1', 'man/xd-users.1', 'man/xd-vboxdnd.1', 'man/xd-wifi-key.1',
  'man/xd-wuaucpl.1',
)
install_man('man/xdg-desktop-portal-xeondesktop.8')
if build_compositor
  install_man('man/xd-compositor.1', 'man/xeondesktop-session.1')
endif
if scenefx_dep.found()
  install_man('man/xd-compositor-fx.1')
endif
