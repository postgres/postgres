# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_visibility_sources = files(
  'pg_visibility.c',
)

if host_system == 'windows'
  pg_visibility_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_visibility',
    '--FILEDESC', 'pg_visibility - page visibility information',])
endif

pg_visibility = shared_module('pg_visibility',
  pg_visibility_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_visibility

install_data(
  'pg_visibility--1.0--1.1.sql',
  'pg_visibility--1.1--1.2.sql',
  'pg_visibility--1.1.sql',
  'pg_visibility.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_visibility',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_visibility',
    ],
  },
  'tap': {
    'tests': [
      't/001_concurrent_transaction.pl',
      't/002_corrupt_vm.pl',
    ],
  },
}
