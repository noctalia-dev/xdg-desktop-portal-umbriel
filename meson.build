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

gnome = import('gnome')

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

gtk4_dep = dependency('gtk4', version: '>=4.12', required: get_option('picker'))

# --- wayland protocol codegen ------------------------------------------------

wayland_scanner = find_program('wayland-scanner')
wp_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')

protocols = {
  'ext-image-capture-source-v1': wp_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
  'ext-image-copy-capture-v1':   wp_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
  'ext-foreign-toplevel-list-v1': wp_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
  'linux-dmabuf-v1':              wp_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml',
}

protocol_sources = []
foreach name, xml : protocols
  protocol_sources += custom_target(
    name + '-client-protocol.h',
    input: xml,
    output: name + '-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_sources += custom_target(
    name + '-protocol.c',
    input: xml,
    output: name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

# --- build-time config substitution -----------------------------------------

conf_data = configuration_data()
conf_data.set('libexecdir', get_option('prefix') / get_option('libexecdir'))

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
  'src/dbus/screenshot.cpp',
  'src/dbus/screencast.cpp',
  'src/dbus/request.cpp',
  'src/dbus/session.cpp',
  'src/wayland/wayland.cpp',
  'src/pipewire/pipewire.cpp',
)

executable(
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

# --- picker ------------------------------------------------------------------

if gtk4_dep.found()
  picker_resources = gnome.compile_resources(
    'picker-resources',
    'src/picker/resources.gresource.xml',
    source_dir: 'src/picker',
  )
  executable(
    'umbriel-share-picker',
    files(
      'src/picker/main.cpp',
      'src/picker/previews.cpp',
      'src/wayland/wayland.cpp',
      'src/loop/loop.cpp',
    ) + picker_resources + protocol_sources,
    include_directories: [include_directories('src'), include_directories('.')],
    dependencies: [gtk4_dep, nlohmann_json_dep, wayland_client_dep, libdrm_dep, dependency('threads')],
    install: true,
    install_dir: get_option('libexecdir'),
  )
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
