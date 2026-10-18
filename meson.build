project(
  'xeondesktop',
  'c',
  version: '0.2.0',
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
  'src/theme/trayicons.c',
  dependencies: [cairo_dep, pango_dep, glib_dep, m_dep],
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
  wlroots_dep = dependency('wlroots-0.20', include_type: 'system')
  xcb_dep = dependency('xcb', include_type: 'system')
  xcb_icccm_dep = dependency('xcb-icccm', include_type: 'system')
  wayland_server_dep = dependency('wayland-server', include_type: 'system')
  libdrm_dep = dependency('libdrm')
  libinput_dep = dependency('libinput', include_type: 'system')
else
  wlroots_dep = disabler()
  xcb_dep = disabler()
  xcb_icccm_dep = disabler()
  wayland_server_dep = disabler()
  libdrm_dep = disabler()
  libinput_dep = disabler()
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
  'xd-window-arranger-v1': files('protocols/xd-window-arranger-v1.xml'),
  'ext-idle-notify-v1': wp_dir / 'staging/ext-idle-notify/ext-idle-notify-v1.xml',
  'ext-session-lock-v1': wp_dir / 'staging/ext-session-lock/ext-session-lock-v1.xml',
  'wlr-output-management-unstable-v1': files('protocols/wlr-output-management-unstable-v1.xml'),
  'wlr-output-power-management-unstable-v1': files(
    'protocols/wlr-output-power-management-unstable-v1.xml',
  ),
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
  # Its attention event names a foreign-toplevel handle, whose interface
  # libwlroots doesn't export.
  custom_target(
    'wlr-foreign-toplevel-server-code',
    input: 'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
    output: 'wlr-foreign-toplevel-management-unstable-v1-server-protocol.c',
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
  'src/compositor/view.c',
  'src/compositor/frame.c',
  'src/compositor/animate.c',
  'src/compositor/snap.c',
  'src/compositor/appicons.c',
  'src/compositor/arrange.c',
  'src/compositor/protocols.c',
  'src/compositor/lock.c',
  'src/compositor/xwayland.c',
  'src/compositor/access.c',
  'src/settings/soundplay.c',
)
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
]
executable(
  'xd-compositor',
  compositor_src,
  server_protocol_headers,
  server_protocol_headers_extra,
  arranger_server_src,
  dependencies: compositor_deps,
  c_args: ['-DWLR_USE_UNSTABLE', '-D_GNU_SOURCE'],
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
    server_protocol_headers,
    server_protocol_headers_extra,
    arranger_server_src,
    dependencies: compositor_deps + [scenefx_dep.as_system()],
    c_args: ['-DWLR_USE_UNSTABLE', '-D_GNU_SOURCE', '-DXD_SCENEFX'],
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
  'src/saver/pipes.c',
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
  'src/apps/desk_saver.c',
  'src/settings/themes.c',
  apps_common,
  dependencies: [ui_dep, saver_dep],
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
# item isn't offered.
archive_dep = dependency('libarchive', required: false, include_type: 'system')
folder_lib = static_library(
  'xdfolder',
  'src/files/control.c',
  'src/files/favorites.c',
  'src/files/fileops.c',
  'src/files/folderview.c',
  'src/files/launcher.c',
  'src/files/places.c',
  'src/files/properties.c',
  'src/files/sendto.c',
  'src/files/undo.c',
  c_args: ['-D_GNU_SOURCE', '-DXD_HAVE_LIBARCHIVE=' + (archive_dep.found() ? '1' : '0')],
  link_with: thumb_lib,
  dependencies: [core_dep, settings_dep, theme_dep, gtk_dep, gio_unix_dep, m_dep, archive_dep],
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
  'data/applications/org.xeondesktop.TaskManager.desktop',
  'data/applications/org.xeondesktop.SystemProperties.desktop',
  'data/applications/org.xeondesktop.DateTime.desktop',
  'data/applications/org.xeondesktop.Display.desktop',
  'data/applications/org.xeondesktop.Taskbar.desktop',
  'data/applications/org.xeondesktop.Sound.desktop',
  'data/applications/org.xeondesktop.Power.desktop',
  'data/applications/org.xeondesktop.RegionalOptions.desktop',
  'data/applications/org.xeondesktop.UserAccounts.desktop',
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
  'data/applications/org.xeondesktop.Printers.desktop',
  'data/applications/org.xeondesktop.Scanners.desktop',
  'data/applications/org.xeondesktop.Search.desktop',
  'data/applications/org.xeondesktop.Help.desktop',
  'data/applications/org.xeondesktop.Defaults.desktop',
  install_dir: get_option('datadir') / 'applications',
)

# xd-taskmgr: the Task Manager (GTK3), with process data from libproc2.
proc2_dep = dependency('libproc2', include_type: 'system')
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
  c_args: ['-D_GNU_SOURCE'],
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

executable(
  'xd-sysdm',
  'src/sysdm/main.c',
  'src/sysdm/hardware.c',
  'src/sysdm/advanced.c',
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
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, crypt_dep],
  c_args: ['-D_GNU_SOURCE'],
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

executable(
  'xd-power',
  'src/power/main.c',
  'src/power/schemes.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep],
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
cairo_ft_dep = dependency('cairo-ft', include_type: 'system')
executable(
  'xd-fonts',
  'src/fonts/main.c',
  'src/fonts/viewer.c',
  'src/fonts/fontlist.c',
  link_with: [gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, fontconfig_dep, cairo_ft_dep],
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
flatpak_dep = dependency('flatpak', include_type: 'system')
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

executable(
  'xd-shell',
  'src/shell/main.c',
  'src/shell/idle.c',
  'src/shell/taskbar.c',
  'src/shell/toplevels.c',
  'src/shell/apps.c',
  'src/files/control.c',
  'src/shell/menu.c',
  'src/shell/startmenu.c',
  'src/shell/deskicons.c',
  'src/shell/desktop.c',
  'src/shell/quicklaunch.c',
  'src/shell/start7.c',
  'src/shell/tooltip.c',
  'src/shell/tray.c',
  'src/shell/dnd.c',
  'src/shell/sys_battery.c',
  'src/shell/sys_access.c',
  'src/shell/sys_bluetooth.c',
  'src/shell/sys_network.c',
  'src/shell/sys_volume.c',
  'src/shell/taskmenu.c',
  'src/shell/recent.c',
  'src/shell/superbar.c',
  'src/shell/jumplist.c',
  'src/shell/sleep.c',
  'src/shell/account.c',
  'src/shell/sounds.c',
  'src/shell/shellbus.c',
  'src/shell/session.c',
  'src/settings/soundplay.c',
  'src/settings/themes.c',
  link_with: rename_lib,
  dependencies: [ui_dep, saver_dep, glib_dep, gio_unix_dep, pulse_dep, canberra_dep, udev_dep],
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

executable('xd-themeview', 'tools/xd-themeview.c', dependencies: [theme_dep])

# Drives a session with --allow-virtual-input, for automated UI tests.
executable(
  'xd-inject',
  'tools/xd-inject.c',
  dependencies: [core_dep, client_protocol_dep, xkbcommon_dep],
  c_args: ['-D_GNU_SOURCE'],
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
  'src/shell/apps.c',
  'src/shell/jumplist.c',
  'src/files/control.c',
  'src/ui/textedit.c',
  'src/apps/runcmd.c',
  'src/security/passwd.c',
  'src/intl/intl.c',
  'src/users/users.c',
  'src/folders/filetypes.c',
  'src/fonts/fontlist.c',
  'src/power/schemes.c',
  'src/keyboard/layouts.c',
  'src/mouse/cursors.c',
  'src/settings/themes.c',
  'src/schedtasks/task.c',
  'src/bluetooth/btinfo.c',
  'src/joy/joyinfo.c',
  'src/netconn/netconn.c',
  'src/admin/admin.c',
  'src/appwiz/programs.c',
  'src/printers/printlib.c',
  'src/scanners/scanlib.c',
  'src/startmenu/searchlib.c',
  'src/files/favorites.c',
  'src/files/launcher.c',
  'src/taskmgr/netstat.c',
  link_with: [thumb_lib, saver_lib],
  dependencies: [theme_dep, settings_dep, glib_dep, gio_unix_dep, gtk_dep, util_dep, crypt_dep,
                 fontconfig_dep, packagekit_dep, cups_dep,
                 sane_dep.partial_dependency(compile_args: true)],
  c_args: ['-DXD_SOURCE_ROOT="' + meson.project_source_root() + '"', '-D_GNU_SOURCE',
           '-DXD_VERSION="' + meson.project_version() + '"'],
)
test(
  'unit',
  test_exe,
  env: [
    'ASAN_OPTIONS=detect_leaks=1',
    'LSAN_OPTIONS=suppressions=' + meson.project_source_root() / 'tests/lsan.supp' + ':print_suppressions=0',
    'UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1',
  ],
)

# UI tests (tests/ui/run.py): each scenario drives a headless session of its
# own. They test what's built, so build first, and they're left out of a
# plain `meson test`:
#   ninja -C build && meson test -C build --setup ui --suite ui
# A failed scenario's screenshots and log go to meson-logs/ui.
python3 = find_program('python3', required: false)
if python3.found()
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
