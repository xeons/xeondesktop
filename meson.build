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

core_lib = static_library(
  'xdcore',
  'src/core/alloc.c',
  'src/core/str.c',
  'src/core/ini.c',
)
core_dep = declare_dependency(link_with: core_lib)

theme_lib = static_library(
  'xdtheme',
  'src/theme/scheme.c',
  'src/theme/draw.c',
  'src/theme/theme.c',
  'src/theme/style_classic.c',
  'src/theme/style_modern.c',
  dependencies: [cairo_dep, pango_dep, m_dep],
)
theme_dep = declare_dependency(
  link_with: theme_lib,
  dependencies: [core_dep, cairo_dep, pango_dep, m_dep],
)

executable('xd-themeview', 'tools/xd-themeview.c', dependencies: [theme_dep])

test_exe = executable(
  'xd-tests',
  'tests/test_main.c',
  dependencies: [theme_dep],
  c_args: ['-DXD_SOURCE_ROOT="' + meson.project_source_root() + '"'],
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
