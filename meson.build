project(
  'timber', 'c',
  version: '0.1',
  default_options: [ 'c_std=c89' ]
)

executable(
  'timber',
  sources: [
    'timber.c',
    configure_file(
      input: 'config.h.in',
      output: 'config.h',
      copy: true,
    ),
  ],
  dependencies: [ dependency('xcb'), dependency('xcb-ewmh') ],
  install: true,
)
