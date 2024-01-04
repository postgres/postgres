# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_trgm_sources = files(
  'trgm_gin.c',
  'trgm_gist.c',
  'trgm_op.c',
  'trgm_regexp.c',
)

if host_system == 'windows'
  pg_trgm_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_trgm',
    '--FILEDESC', 'pg_trgm - trigram matching',])
endif

pg_trgm = shared_module('pg_trgm',
  pg_trgm_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_trgm

install_data(
  'pg_trgm--1.0--1.1.sql',
  'pg_trgm--1.1--1.2.sql',
  'pg_trgm--1.2--1.3.sql',
  'pg_trgm--1.3--1.4.sql',
  'pg_trgm--1.3.sql',
  'pg_trgm--1.4--1.5.sql',
  'pg_trgm--1.5--1.6.sql',
  'pg_trgm.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_trgm',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_trgm',
      'pg_word_trgm',
      'pg_strict_word_trgm',
    ],
  },
}
