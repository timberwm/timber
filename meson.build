project(
  'timber', 'c',
  version: '0.15',
  license: 'GPL-3.0-or-later',
  meson_version: '>=0.61.2',
  default_options: [
    'c_std=c11',
    'warning_level=3',
  ]
)

subdir('proto')
subdir('src')
subdir('docs')

summary({
  'Configuration path': get_option('config_path'),
}, section: 'Configuration')
