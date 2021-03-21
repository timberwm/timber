config = configuration_data()
config.set('version', meson.project_version())

manpage = configure_file(
  input: 'timber.1.in',
  output: 'timber.1',
  configuration: config,
)

install_man(manpage)
