project(
  'timber', 'c',
  version: '0.1',
  default_options: [ 'c_std=c89' ]
)

config = configuration_data()
config.set('version', meson.project_version())

executable(
  'timber',
  sources: [
    'timber.c',
    configure_file(
      input: 'config.h.in',
      output: 'config.h',
      configuration: config
    ),
  ],
  dependencies: [
    dependency('xcb'),
    dependency('xcb-aux'),
    dependency('xcb-randr')
  ],
  install: true,
)
