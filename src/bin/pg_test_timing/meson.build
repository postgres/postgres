# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_test_timing_sources = files(
  'pg_test_timing.c'
)

if host_system == 'windows'
  pg_test_timing_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_test_timing',
    '--FILEDESC', 'pg_test_timing - test timing overhead'])
endif

pg_test_timing = executable('pg_test_timing',
  pg_test_timing_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_test_timing

tests += {
  'name': 'pg_test_timing',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}

subdir('po', if_found: libintl)
