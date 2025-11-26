# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_datemath_sources = files(
  'pg_datemath.c',
)

if host_system == 'windows'
  pg_datemath_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_datemath',
    '--FILEDESC', 'pg_datemath - Enhanced date difference functions',])
endif

pg_datemath = shared_module('pg_datemath',
  pg_datemath_sources,
  kwargs: contrib_mod_args,
)

contrib_targets += pg_datemath

install_data(
  'pg_datemath--1.0.sql',
  'pg_datemath.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_datemath',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_datemath',
    ],
  },
}

