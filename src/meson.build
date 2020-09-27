wayland_protocols = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
wayland_scanner = dependency('wayland-scanner').get_variable(pkgconfig: 'wayland_scanner')

xdg_shell_protocol_h = custom_target(
  'xdg-shell-protocol.h',
  input: join_paths(wayland_protocols, 'stable', 'xdg-shell', 'xdg-shell.xml'),
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_protocol_c = custom_target(
  'xdg-shell-protocol.c',
  input: join_paths(wayland_protocols, 'stable', 'xdg-shell', 'xdg-shell.xml'),
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

config = configuration_data()
config.set('version', meson.project_version())
config_header = configure_file(
  input: 'config.h.in',
  output: 'config.h',
  configuration: config,
)

executable(
  'timber',
  sources: [
      'client.c',
      'common.c',
      'timber.c',
      'wm.c',
      config_header,
      xdg_shell_protocol_h,
      xdg_shell_protocol_c,
  ],
  dependencies: [
    dependency('glesv2'),
    dependency('pixman-1'),
    dependency('wayland-server'),
    dependency('wlroots'),
    dependency('xkbcommon'),
  ],
  c_args: [
    '-DWLR_USE_UNSTABLE',
    '-D_POSIX_C_SOURCE=200809L',
  ],
  install: true,
)
