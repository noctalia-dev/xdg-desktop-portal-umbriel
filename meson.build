project(
  'xdg-desktop-portal-umbriel',
  ['c', 'cpp'],
  version: '0.1.0',
  meson_version: '>=1.3',
  default_options: [
    'cpp_std=c++23',
    'warning_level=2',
    'werror=false',
    'b_ndebug=if-release',
  ],
)

# --- dependencies -----------------------------------------------------------

sdbus_dep = dependency('sdbus-c++', version: '>=2.0')
pipewire_dep = dependency('libpipewire-0.3')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols', version: '>=1.39')
libdrm_dep = dependency('libdrm')
gbm_dep = dependency('gbm')
cairo_dep = dependency('cairo')
tomlplusplus_dep = dependency('tomlplusplus')
nlohmann_json_dep = dependency('nlohmann_json')

# The share picker uses the noctalia-ui copy in lib/ (refreshed by tools/sync-lib.sh).
if get_option('picker')
  # Recent GLib headers use __COUNTER__, a C2y extension clang warns about under lib/'s -Wpedantic. lib/ adds
  # GLib with -I after project arguments, so mark its include directory as a system directory (-isystem wins).
  add_project_arguments('-isystem', dependency('glib-2.0').get_variable(pkgconfig: 'includedir') / 'glib-2.0', language: 'cpp')
  subdir('lib')
endif

# --- wayland protocol codegen ------------------------------------------------

wayland_scanner = find_program('wayland-scanner')
wp_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')

protocols = {
  'ext-image-capture-source-v1': wp_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  'ext-image-copy-capture-v1':   wp_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  'ext-foreign-toplevel-list-v1': wp_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  'linux-dmabuf-v1':              wp_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
  'xdg-output-unstable-v1':       wp_dir / 'unstable/xdg-output/xdg-output-unstable-v1.xml',
  'umbriel-input-capture-v1':     meson.current_source_dir() / 'protocols/umbriel-input-capture-v1.xml',
  'umbriel-global-shortcuts-v1':  meson.current_source_dir() / 'protocols/umbriel-global-shortcuts-v1.xml',
}

protocol_sources = []
# The picker links lib/, which already defines the xdg-output interface code.
protocol_headers = []
picker_protocol_code = []
foreach name, xml : protocols
  header = custom_target(
    name + '-client-protocol.h',
    input: xml,
    output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  code = custom_target(
    name + '-protocol.c',
    input: xml,
    output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  protocol_sources += [header, code]
  protocol_headers += header
  if name != 'xdg-output-unstable-v1'
    picker_protocol_code += code
  endif
endforeach

# --- build-time config substitution -----------------------------------------

conf_data = configuration_data()
conf_data.set('libexecdir', get_option('prefix') / get_option('libexecdir'))
conf_data.set('pickerassetsdir', get_option('prefix') / get_option('datadir') / 'xdg-desktop-portal-umbriel' / 'assets')

defaults_h = configure_file(
  input: 'src/config/defaults.h.in',
  output: 'defaults.h',
  configuration: conf_data,
)

# --- daemon ------------------------------------------------------------------

daemon_sources = files(
  'src/main.cpp',
  'src/loop/loop.cpp',
  'src/config/config.cpp',
  'src/dbus/dbus.cpp',
  'src/dbus/input_capture.cpp',
  'src/dbus/global_shortcuts.cpp',
  'src/dbus/global_shortcuts_credentials.cpp',
  'src/dbus/screenshot.cpp',
  'src/dbus/screencast.cpp',
  'src/dbus/request.cpp',
  'src/dbus/session.cpp',
  'src/wayland/wayland.cpp',
  'src/wayland/global_shortcuts.cpp',
  'src/pipewire/pipewire.cpp',
  'src/umbriel/ipc.cpp',
)

portal_daemon = executable(
  'xdg-desktop-portal-umbriel',
  daemon_sources + protocol_sources + [defaults_h],
  include_directories: [include_directories('src'), include_directories('.')],
  dependencies: [
    sdbus_dep,
    pipewire_dep,
    wayland_client_dep,
    libdrm_dep,
    gbm_dep,
    cairo_dep,
    tomlplusplus_dep,
    nlohmann_json_dep,
  ],
  install: true,
  install_dir: get_option('libexecdir'),
)

# --- tests -------------------------------------------------------------------

if get_option('tests')
  capture_transform_test = executable(
    'capture-transform-test',
    'tests/capture_transform.cpp',
    include_directories: include_directories('src'),
  )
  test('capture-transform', capture_transform_test)

  global_shortcuts_server_h = custom_target(
    'global-shortcuts-server-protocol.h',
    input: protocols['umbriel-global-shortcuts-v1'],
    output: 'global-shortcuts-server-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )
  global_shortcuts_client_test = executable(
    'global-shortcuts-client-test',
    files(
      'tests/global_shortcuts_client.cpp',
      'src/wayland/global_shortcuts.cpp',
      'src/wayland/wayland.cpp',
      'src/loop/loop.cpp',
    ) + protocol_sources + [global_shortcuts_server_h],
    include_directories: [include_directories('src'), include_directories('.')],
    dependencies: [wayland_client_dep, dependency('wayland-server'), libdrm_dep, nlohmann_json_dep],
  )
  test('global-shortcuts-client', global_shortcuts_client_test)

  global_shortcuts_credentials_test = executable(
    'global-shortcuts-credentials-test',
    files('tests/global_shortcuts_credentials.cpp', 'src/dbus/global_shortcuts_credentials.cpp'),
    include_directories: include_directories('src'),
    dependencies: nlohmann_json_dep,
    link_args: ['-Wl,--wrap=write', '-Wl,--wrap=fsync', '-Wl,--wrap=renameat'],
  )
  test('global-shortcuts-credentials', global_shortcuts_credentials_test)

  global_shortcuts_portal_test = executable(
    'global-shortcuts-portal-test',
    files(
      'tests/global_shortcuts_portal.cpp',
      'src/dbus/global_shortcuts.cpp',
      'src/dbus/request.cpp',
      'src/dbus/screenshot.cpp',
      'src/dbus/global_shortcuts_credentials.cpp',
      'src/wayland/global_shortcuts.cpp',
      'src/wayland/wayland.cpp',
      'src/loop/loop.cpp',
    ) + protocol_sources + [global_shortcuts_server_h, defaults_h],
    include_directories: [include_directories('src'), include_directories('.')],
    dependencies: [sdbus_dep, wayland_client_dep, dependency('wayland-server'), libdrm_dep, nlohmann_json_dep, cairo_dep],
    link_args: ['-Wl,--wrap=renameat'],
  )
  dbus_session_config = meson.project_source_root() / 'tests/dbus-session.conf'
  test('global-shortcuts-portal', find_program('dbus-run-session'), args: ['--config-file=' + dbus_session_config, '--', global_shortcuts_portal_test])

  daemon_shutdown_test = executable(
    'daemon-shutdown-test',
    files('tests/daemon_shutdown.cpp', 'src/dbus/global_shortcuts_credentials.cpp') + protocol_sources + [global_shortcuts_server_h],
    include_directories: [include_directories('src'), include_directories('.')],
    dependencies: [sdbus_dep, wayland_client_dep, dependency('wayland-server'), nlohmann_json_dep],
  )
  test('daemon-shutdown', find_program('dbus-run-session'), args: ['--config-file=' + dbus_session_config, '--', daemon_shutdown_test, portal_daemon])

  executable(
    'screencast-test-client',
    'tests/screencast_test_client.cpp',
    dependencies: [sdbus_dep, pipewire_dep],
  )

  executable(
    'global-shortcuts-test-client',
    'tests/global_shortcuts_portal_client.cpp',
    dependencies: [sdbus_dep, nlohmann_json_dep],
  )

  libei_test_dep = dependency('libei-1.0', version: '>=1.3')
  executable(
    'input-capture-test-client',
    'tests/input_capture_client.cpp',
    dependencies: [sdbus_dep, libei_test_dep],
  )

  buffer_params_test = executable(
    'pipewire-buffer-params-test',
    'tests/pipewire_buffer_params.cpp',
    include_directories: include_directories('src'),
    dependencies: [pipewire_dep, wayland_client_dep, libdrm_dep, gbm_dep],
    # The test includes pipewire.cpp, so GCC treats its anonymous-namespace types as header types.
    cpp_args: ['-ffunction-sections', '-fdata-sections'] + meson.get_compiler('cpp').get_supported_arguments('-Wno-subobject-linkage'),
    link_args: [
      '-Wl,--gc-sections',
      '-Wl,--wrap=gbm_device_get_format_modifier_plane_count',
      '-Wl,--wrap=gbm_bo_create_with_modifiers2',
      '-Wl,--wrap=gbm_bo_create_with_modifiers',
      '-Wl,--wrap=gbm_bo_create',
      '-Wl,--wrap=gbm_bo_get_modifier',
      '-Wl,--wrap=gbm_bo_get_plane_count',
      '-Wl,--wrap=gbm_bo_destroy',
      '-Wl,--wrap=gbm_device_destroy',
      '-Wl,--wrap=pw_stream_update_params',
    ],
  )
  test('pipewire-buffer-params', buffer_params_test)

  if get_option('picker')
    picker_theme_test = executable(
      'picker-theme-test',
      files('tests/picker_theme.cpp', 'src/picker/theme.cpp'),
      include_directories: include_directories('src'),
      dependencies: [noctalia_ui_dep, nlohmann_json_dep],
    )
    test('picker-theme', picker_theme_test)
  endif
endif

# --- picker ------------------------------------------------------------------

if get_option('picker')
  xdg_decoration_xml = wp_dir / 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'
  xdg_decoration_h = custom_target(
    'xdg-decoration-unstable-v1-client-protocol.h',
    input: xdg_decoration_xml,
    output: 'xdg-decoration-unstable-v1-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  xdg_decoration_c = custom_target(
    'xdg-decoration-unstable-v1-protocol.c',
    input: xdg_decoration_xml,
    output: 'xdg-decoration-unstable-v1-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  executable(
    'umbriel-share-picker',
    files(
      'src/picker/main.cpp',
      'src/picker/decoration.cpp',
      'src/picker/main_loop.cpp',
      'src/picker/picker_window.cpp',
      'src/picker/previews.cpp',
      'src/picker/request.cpp',
      'src/picker/theme.cpp',
      'src/wayland/wayland.cpp',
      'src/loop/loop.cpp',
    ) + protocol_headers + picker_protocol_code + [xdg_decoration_h, xdg_decoration_c, defaults_h],
    include_directories: [include_directories('src'), include_directories('.')],
    dependencies: [noctalia_ui_dep, nlohmann_json_dep, wayland_client_dep, libdrm_dep, dependency('threads')],
    install: true,
    install_dir: get_option('libexecdir'),
  )
  # The glyph font and its metadata, which the renderer loads at startup.
  install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: get_option('datadir') / 'xdg-desktop-portal-umbriel' / 'assets')
endif

# --- data files --------------------------------------------------------------

install_data('data/umbriel.portal',
  install_dir: get_option('datadir') / 'xdg-desktop-portal' / 'portals')

install_data('data/umbriel-portals.conf',
  install_dir: get_option('datadir') / 'xdg-desktop-portal')

configure_file(
  input: 'data/org.freedesktop.impl.portal.desktop.umbriel.service.in',
  output: 'org.freedesktop.impl.portal.desktop.umbriel.service',
  configuration: conf_data,
  install_dir: get_option('datadir') / 'dbus-1' / 'services',
)

configure_file(
  input: 'data/xdg-desktop-portal-umbriel.service.in',
  output: 'xdg-desktop-portal-umbriel.service',
  configuration: conf_data,
  install_dir: get_option('prefix') / 'lib' / 'systemd' / 'user',
)
