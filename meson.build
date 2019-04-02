project(
  'timber', 'c',
  version: '0.1',
  default_options: [ 'c_std=c89' ]
)

config = configuration_data()
config.set('version', meson.project_version())
config_header = configure_file(input: 'config.h.in', output: 'config.h', configuration: config)

executable(
  'timber',
  sources: [ 'timber.c', config_header ],
  dependencies: [
    dependency('xcb'),
    dependency('xcb-aux'),
    dependency('xcb-randr')
  ],
  install: true,
)
