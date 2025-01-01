# Copyright (c) 2022-2025, PostgreSQL Global Development Group

citext_sources = files(
  'citext.c',
)

if host_system == 'windows'
  citext_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'citext',
    '--FILEDESC', 'citext - case-insensitive character string data type',])
endif

citext = shared_module('citext',
  citext_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += citext

install_data(
  'citext.control',
  'citext--1.0--1.1.sql',
  'citext--1.1--1.2.sql',
  'citext--1.2--1.3.sql',
  'citext--1.3--1.4.sql',
  'citext--1.4.sql',
  'citext--1.4--1.5.sql',
  'citext--1.5--1.6.sql',
  'citext--1.6--1.7.sql',
  'citext--1.7--1.8.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'citext',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'create_index_acl',
      'citext',
      'citext_utf8',
    ],
  },
}
