# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_config_sources = files(
  'pg_config.c',
)

if host_system == 'windows'
  pg_config_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_config',
    '--FILEDESC', 'pg_config - report configuration information',])
endif

pg_config = executable('pg_config',
  pg_config_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_config

tests += {
  'name': 'pg_config',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_pg_config.pl',
    ],
  },
}

subdir('po', if_found: libintl)
