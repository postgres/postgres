# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_buffercache_sources = files(
  'pg_buffercache_pages.c',
)

if host_system == 'windows'
  pg_buffercache_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_buffercache',
    '--FILEDESC', 'pg_buffercache - monitoring of shared buffer cache in real-time',])
endif

pg_buffercache = shared_module('pg_buffercache',
  pg_buffercache_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_buffercache

install_data(
  'pg_buffercache--1.0--1.1.sql',
  'pg_buffercache--1.1--1.2.sql',
  'pg_buffercache--1.2--1.3.sql',
  'pg_buffercache--1.2.sql',
  'pg_buffercache--1.3--1.4.sql',
  'pg_buffercache--1.4--1.5.sql',
  'pg_buffercache.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_buffercache',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_buffercache',
    ],
  },
}
