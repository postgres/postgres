# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pg_stash_advice_sources = files(
  'pg_stash_advice.c',
  'stashfuncs.c',
  'stashpersist.c'
)

if host_system == 'windows'
  pg_stash_advice_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_stash_advice',
    '--FILEDESC', 'pg_stash_advice - store and automatically apply plan advice',])
endif

pg_stash_advice = shared_module('pg_stash_advice',
  pg_stash_advice_sources,
  include_directories: [pg_plan_advice_inc, include_directories('.')],
  kwargs: contrib_mod_args,
)
contrib_targets += pg_stash_advice

install_data(
  'pg_stash_advice--1.0.sql',
  'pg_stash_advice.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_stash_advice',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_stash_advice',
      'pg_stash_advice_utf8',
    ],
  },
  'tap': {
    'tests': [
      't/001_persist.pl',
    ],
  },
}
