project(
  'timber', 'c',
  version: '0.14',
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
  'Border width': get_option('border_width'),
  'Border color (active)': get_option('border_color_active'),
  'Border color (inactive)': get_option('border_color_inactive'),
}, section: 'Configuration')
