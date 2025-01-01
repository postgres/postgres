# Copyright (c) 2022-2025, PostgreSQL Global Development Group

basic_archive_sources = files(
  'basic_archive.c',
)

if host_system == 'windows'
  basic_archive_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'basic_archive',
    '--FILEDESC', 'basic_archive - basic archive module',])
endif

basic_archive = shared_module('basic_archive',
  basic_archive_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += basic_archive

tests += {
  'name': 'basic_archive',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'basic_archive',
    ],
    'regress_args': [
      '--temp-config', files('basic_archive.conf'),
    ],
    # Disabled because these tests require special configuration, which
    # typical installcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
}
