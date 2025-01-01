# Copyright (c) 2022-2025, PostgreSQL Global Development Group

bloom_sources = files(
  'blcost.c',
  'blinsert.c',
  'blscan.c',
  'blutils.c',
  'blvacuum.c',
  'blvalidate.c',
)

if host_system == 'windows'
  bloom_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'bloom',
    '--FILEDESC', 'bloom access method - signature file based index',])
endif

bloom = shared_module('bloom',
  bloom_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += bloom

install_data(
  'bloom.control',
  'bloom--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'bloom',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'bloom',
    ],
  },
  'tap': {
    'tests': [
      't/001_wal.pl',
    ],
  },
}
