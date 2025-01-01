# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_walsummary_sources = files(
  'pg_walsummary.c',
)

if host_system == 'windows'
  pg_walsummary_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_walsummary',
    '--FILEDESC', 'pg_walsummary - print contents of WAL summary files',])
endif

pg_walsummary = executable('pg_walsummary',
  pg_walsummary_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_walsummary

tests += {
  'name': 'pg_walsummary',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_blocks.pl',
    ],
  }
}

subdir('po', if_found: libintl)
