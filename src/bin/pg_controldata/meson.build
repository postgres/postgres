# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_controldata_sources = files(
  'pg_controldata.c',
)

if host_system == 'windows'
  pg_controldata_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_controldata',
    '--FILEDESC', 'pg_controldata - reads the data from pg_control',])
endif

pg_controldata = executable('pg_controldata',
  pg_controldata_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_controldata

tests += {
  'name': 'pg_controldata',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_pg_controldata.pl',
    ],
  },
}

subdir('po', if_found: libintl)
