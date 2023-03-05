# Copyright (c) 2022-2023, PostgreSQL Global Development Group

fuzzystrmatch_sources = files(
  'fuzzystrmatch.c',
  'dmetaphone.c',
)

if host_system == 'windows'
  fuzzystrmatch_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'fuzzystrmatch',
    '--FILEDESC', 'fuzzystrmatch - similarities and distance between strings',])
endif

fuzzystrmatch = shared_module('fuzzystrmatch',
  fuzzystrmatch_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += fuzzystrmatch

install_data(
  'fuzzystrmatch.control',
  'fuzzystrmatch--1.0--1.1.sql',
  'fuzzystrmatch--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'fuzzystrmatch',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'fuzzystrmatch',
    ],
  },
}
