# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_resetwal_sources = files(
  'pg_resetwal.c',
)

if host_system == 'windows'
  pg_resetwal_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_resetwal',
    '--FILEDESC', 'pg_resetwal - reset PostgreSQL WAL log'])
endif

pg_resetwal = executable('pg_resetwal',
  pg_resetwal_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_resetwal

tests += {
  'name': 'pg_resetwal',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_corrupted.pl',
    ],
  },
}

subdir('po', if_found: libintl)
