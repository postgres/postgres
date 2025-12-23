# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_prewarm_sources = files(
  'autoprewarm.c',
  'pg_prewarm.c',
)

if host_system == 'windows'
  pg_prewarm_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_prewarm',
    '--FILEDESC', 'pg_prewarm - preload relation data into system buffer cache',])
endif

pg_prewarm = shared_module('pg_prewarm',
  pg_prewarm_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_prewarm

install_data(
  'pg_prewarm--1.0--1.1.sql',
  'pg_prewarm--1.1--1.2.sql',
  'pg_prewarm--1.1.sql',
  'pg_prewarm.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_prewarm',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_prewarm',
    ],
  },
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}
