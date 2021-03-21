wayland_protocols = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
wayland_scanner = dependency('wayland-scanner').get_variable(pkgconfig: 'wayland_scanner')

proto_sources = []

proto_sources += custom_target(
  'timber-server.h',
  input: 'timber.xml',
  output: 'timber-server.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

proto_sources += custom_target(
  'timber-client-header.h',
  input: 'timber.xml',
  output: 'timber-client.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

proto_sources += custom_target(
  'timber-private-code.c',
  input: 'timber.xml',
  output: 'timber-private-code.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

proto_sources += custom_target(
  'xdg-shell-protocol.h',
  input: join_paths(wayland_protocols, 'stable', 'xdg-shell', 'xdg-shell.xml'),
  output: 'xdg-shell-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

proto_sources += custom_target(
  'xdg-shell-protocol.c',
  input: join_paths(wayland_protocols, 'stable', 'xdg-shell', 'xdg-shell.xml'),
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

proto_sources += custom_target(
  'wlr-layer-shell-unstable-v1-protocol.h',
  input: 'wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

proto_sources += custom_target(
  'wlr-layer-shell-unstable-v1-protocol.c',
  input: 'wlr-layer-shell-unstable-v1.xml',
  output: 'wlr-layer-shell-unstable-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
