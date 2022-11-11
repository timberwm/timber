version = meson.project_version()

git = find_program('git', required : false)
if git.found()
  git_version = run_command(git, '-C', meson.project_source_root(), 'describe', check: false)
  if git_version.returncode() == 0
    version = git_version.stdout().strip()
  endif
endif

executable(
  'timber',
  sources: [
      'client.c',
      'common.c',
      'timber.c',
      'timber.h',
      'wm.c',
      proto_sources,
  ],
  dependencies: [
    dependency('wayland-client'),
    dependency('wayland-server'),
    dependency('wlroots', version: '>=0.16.0'),
    dependency('xkbcommon'),
  ],
  c_args: [
    '-DWLR_USE_UNSTABLE',
    '-D_POSIX_C_SOURCE=200809L',
    '-DTMBR_VERSION="' + version + '"',
    '-DTMBR_CONFIG_PATH="' + get_option('config_path') + '"',
    '-DTMBR_BORDER_WIDTH=' + get_option('border_width').to_string(),
    '-DTMBR_COLOR_ACTIVE=((float[4])' + get_option('border_color_active') + ')',
    '-DTMBR_COLOR_INACTIVE=((float[4])' + get_option('border_color_inactive') + ')',
  ],
  install: true,
)
