project(
  'timber', 'c',
  version: '0.6',
  default_options: [ 'c_std=c89' ]
)

add_global_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

config = configuration_data()
config.set('version', meson.project_version())
config_header = configure_file(input: 'src/config.h.in', output: 'config.h', configuration: config)

executable(
  'timber',
  sources: [
      'src/common.c',
      'src/timber.c',
      'src/wm.c',
      config_header
  ],
  dependencies: [
    dependency('xcb'),
    dependency('xcb-aux'),
    dependency('xcb-randr')
  ],
  install: true,
)
