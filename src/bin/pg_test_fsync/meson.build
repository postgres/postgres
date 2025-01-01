# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_fsync_sources = files(
  'pg_test_fsync.c',
)

if host_system == 'windows'
  test_fsync_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_test_fsync',
    '--FILEDESC', 'pg_test_fsync - test various disk sync methods'])
endif

pg_test_fsync = executable('pg_test_fsync',
  test_fsync_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_test_fsync

tests += {
  'name': 'pg_test_fsync',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}

subdir('po', if_found: libintl)
