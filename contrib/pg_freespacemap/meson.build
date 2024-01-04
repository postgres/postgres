# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_freespacemap_sources = files(
  'pg_freespacemap.c',
)

if host_system == 'windows'
  pg_freespacemap_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_freespacemap',
    '--FILEDESC', 'pg_freespacemap - monitoring of free space map',])
endif

pg_freespacemap = shared_module('pg_freespacemap',
  pg_freespacemap_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_freespacemap

install_data(
  'pg_freespacemap--1.0--1.1.sql',
  'pg_freespacemap--1.1--1.2.sql',
  'pg_freespacemap--1.1.sql',
  'pg_freespacemap.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_freespacemap',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_freespacemap',
    ],
    'regress_args': [
      '--temp-config', files('pg_freespacemap.conf')
    ],
    # Disabled because these tests require "autovacuum=off", which
    # typical runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  },
}
