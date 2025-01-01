# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_archivecleanup_sources = files(
  'pg_archivecleanup.c',
)

if host_system == 'windows'
  pg_archivecleanup_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_archivecleanup',
    '--FILEDESC', 'pg_archivecleanup - cleans archive when used with streaming replication',])
endif

pg_archivecleanup = executable('pg_archivecleanup',
  pg_archivecleanup_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_archivecleanup

tests += {
  'name': 'pg_archivecleanup',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/010_pg_archivecleanup.pl',
    ],
  },
}

subdir('po', if_found: libintl)
