config_header = vcs_tag(
  input: 'config.h.in',
  output: 'config.h',
)

executable(
  'timber',
  sources: [
      'client.c',
      'common.c',
      'timber.c',
      'timber.h',
      'wm.c',
      config_header,
      proto_sources,
  ],
  dependencies: [
    dependency('glesv2'),
    dependency('pixman-1'),
    dependency('wayland-client'),
    dependency('wayland-server'),
    dependency('wlroots', version: '>=0.11.0'),
    dependency('xkbcommon'),
  ],
  c_args: [
    '-DWLR_USE_UNSTABLE',
    '-D_POSIX_C_SOURCE=200809L',
  ],
  install: true,
)
