add_global_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

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
      config_header
  ],
  dependencies: [
    dependency('xcb'),
    dependency('xcb-aux'),
    dependency('xcb-randr')
  ],
  install: true,
)
