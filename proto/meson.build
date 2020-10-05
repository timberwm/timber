wayland_scanner = dependency('wayland-scanner').get_variable(pkgconfig: 'wayland_scanner')

timber_server_header = custom_target(
  'timber-server.h',
  input: 'timber.xml',
  output: 'timber-server.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
)

timber_client_header = custom_target(
  'timber-client-header.h',
  input: 'timber.xml',
  output: 'timber-client.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

timber_private_code = custom_target(
  'timber-private-code.c',
  input: 'timber.xml',
  output: 'timber-private-code.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

