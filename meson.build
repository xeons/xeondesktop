project(
  'xeondesktop',
  'c',
  version: '0.1.0',
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
fontconfig_dep = dependency('fontconfig', include_type: 'system')
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
  'src/theme/style_whistler.c',
  'src/theme/luna.c',
  'src/theme/gtkcss.c',
  dependencies: [cairo_dep, pango_dep, fontconfig_dep, glib_dep, m_dep],
)
theme_dep = declare_dependency(
  link_with: theme_lib,
  dependencies: [core_dep, cairo_dep, pango_dep, fontconfig_dep, glib_dep, m_dep],
)

settings_lib = static_library(
  'xdsettings',
  'src/settings/settings.c',
  'src/settings/state.c',
  'src/settings/gtktheme.c',
  dependencies: [cairo_dep, pango_dep, glib_dep, m_dep],
  c_args: ['-D_GNU_SOURCE'],
)
settings_dep = declare_dependency(link_with: settings_lib, dependencies: [theme_dep, glib_dep])

wlroots_dep = dependency('wlroots-0.20', include_type: 'system')
xcb_dep = dependency('xcb', include_type: 'system')
xcb_icccm_dep = dependency('xcb-icccm', include_type: 'system')
wayland_server_dep = dependency('wayland-server', include_type: 'system')
wayland_client_dep = dependency('wayland-client', include_type: 'system')
xkbcommon_dep = dependency('xkbcommon', include_type: 'system')
libdrm_dep = dependency('libdrm', include_type: 'system').partial_dependency(compile_args: true)

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
  'wlr-layer-shell-unstable-v1': layer_shell_xml,
  'wlr-foreign-toplevel-management-unstable-v1': files(
    'protocols/wlr-foreign-toplevel-management-unstable-v1.xml',
  ),
  'wlr-virtual-pointer-unstable-v1': files('protocols/wlr-virtual-pointer-unstable-v1.xml'),
  'virtual-keyboard-unstable-v1': files('protocols/virtual-keyboard-unstable-v1.xml'),
  'xd-window-arranger-v1': files('protocols/xd-window-arranger-v1.xml'),
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

# wlroots' output power header includes this generated server header; the
# XML comes from the wlr-protocols package.
wlr_protocols_dir = dependency('wlr-protocols').get_variable('pkgdatadir')
server_protocol_headers_extra = [
  custom_target(
    'wlr-output-power-management-server-header',
    input: wlr_protocols_dir / 'unstable/wlr-output-power-management-unstable-v1.xml',
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

executable(
  'xd-compositor',
  'src/compositor/main.c',
  'src/compositor/output.c',
  'src/compositor/layers.c',
  'src/compositor/input.c',
  'src/compositor/view.c',
  'src/compositor/frame.c',
  'src/compositor/arrange.c',
  'src/compositor/protocols.c',
  'src/compositor/lock.c',
  'src/compositor/xwayland.c',
  server_protocol_headers,
  server_protocol_headers_extra,
  arranger_server_src,
  dependencies: [
    theme_dep,
    settings_dep,
    wlroots_dep,
    xcb_dep,
    xcb_icccm_dep,
    wayland_server_dep,
    xkbcommon_dep,
    libdrm_dep,
  ],
  c_args: ['-DWLR_USE_UNSTABLE', '-D_GNU_SOURCE'],
  install: true,
)

gtk_dep = dependency('gtk+-3.0', include_type: 'system')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0', include_type: 'system')
gio_unix_dep = dependency('gio-unix-2.0', include_type: 'system')

# libxdui: Wayland client plumbing and dialog widgets for shell programs.
ui_lib = static_library(
  'xdui',
  'src/ui/shm.c',
  'src/ui/icons.c',
  'src/ui/textedit.c',
  'src/ui/image.c',
  'src/ui/ui.c',
  'src/ui/widgets.c',
  dependencies: [theme_dep, settings_dep, client_protocol_dep, gtk_dep, gdk_pixbuf_dep,
                 xkbcommon_dep],
  c_args: ['-D_GNU_SOURCE'],
)
ui_dep = declare_dependency(
  link_with: ui_lib,
  dependencies: [theme_dep, settings_dep, client_protocol_dep, gtk_dep, gdk_pixbuf_dep,
                 xkbcommon_dep],
)

apps_common = files('src/apps/common.c')

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
  apps_common,
  dependencies: [ui_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable(
  'xd-desk',
  'src/apps/desk.c',
  dependencies: [ui_dep],
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

# xd-files: the file manager (GTK3), with the folder view (libxdfolder).
folder_lib = static_library(
  'xdfolder',
  'src/files/folderview.c',
  'src/files/properties.c',
  c_args: ['-D_GNU_SOURCE'],
  dependencies: [core_dep, gtk_dep, m_dep],
)
gtkapp_lib = static_library(
  'xdgtkapp',
  'src/gtkapp/gtkapp.c',
  dependencies: [settings_dep, gtk_dep],
)
install_data(
  'data/applications/org.xeondesktop.Files.desktop',
  'data/applications/org.xeondesktop.TaskManager.desktop',
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
  c_args: ['-D_GNU_SOURCE'],
  link_with: gtkapp_lib,
  dependencies: [core_dep, settings_dep, gtk_dep, gio_unix_dep, proc2_dep, client_protocol_dep, m_dep],
  install: true,
)
executable(
  'xd-files',
  'src/files/main.c',
  link_with: [folder_lib, gtkapp_lib],
  dependencies: [core_dep, settings_dep, gtk_dep, m_dep],
  install: true,
)

executable(
  'xd-shell',
  'src/shell/main.c',
  'src/shell/taskbar.c',
  'src/shell/toplevels.c',
  'src/shell/apps.c',
  'src/shell/menu.c',
  'src/shell/startmenu.c',
  'src/shell/desktop.c',
  'src/shell/quicklaunch.c',
  'src/shell/start7.c',
  'src/shell/taskmenu.c',
  'src/shell/recent.c',
  dependencies: [ui_dep, glib_dep, gio_unix_dep],
  c_args: ['-D_GNU_SOURCE'],
  install: true,
)

executable('xd-themeview', 'tools/xd-themeview.c', dependencies: [theme_dep])

# Drives a session with --allow-virtual-input, for automated UI tests.
executable(
  'xd-inject',
  'tools/xd-inject.c',
  dependencies: [core_dep, client_protocol_dep, xkbcommon_dep],
  c_args: ['-D_GNU_SOURCE'],
)

test_exe = executable(
  'xd-tests',
  'tests/test_main.c',
  'src/shell/apps.c',
  'src/ui/textedit.c',
  'src/apps/runcmd.c',
  dependencies: [theme_dep, settings_dep, glib_dep, gio_unix_dep, gtk_dep],
  c_args: ['-DXD_SOURCE_ROOT="' + meson.project_source_root() + '"', '-D_GNU_SOURCE'],
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

install_subdir('data/schemes', install_dir: get_option('datadir') / 'xeondesktop')
