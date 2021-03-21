wayland_protocols = dependency('wayland-protocols').get_variable(pkgconfig: 'pkgdatadir')
wayland_scanner = dependency('wayland-scanner').get_variable(pkgconfig: 'wayland_scanner')

protos = {
  'timber.xml': true,
  'wlr-layer-shell-unstable-v1.xml': false,
  join_paths(wayland_protocols, 'stable', 'xdg-shell', 'xdg-shell.xml'): false,
}
proto_sources = []

foreach proto, generate_client_header : protos
  proto_sources += custom_target(proto.underscorify() + '_protocol_h',
    input: proto,
    output: '@BASENAME@-protocol.h',
    command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
  )

  proto_sources += custom_target(proto.underscorify() + '_protocol_c',
    input: proto,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )

  if generate_client_header
    proto_sources += custom_target(proto.underscorify() + '_client_protocol_h',
      input: proto,
      output: '@BASENAME@-client-protocol.h',
      command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
      )
  endif
endforeach
