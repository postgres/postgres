# Copyright (c) 2022-2025, PostgreSQL Global Development Group

fuzzystrmatch_sources = files(
  'daitch_mokotoff.c',
  'dmetaphone.c',
  'fuzzystrmatch.c',
)

daitch_mokotoff_h = custom_target('daitch_mokotoff',
  input: 'daitch_mokotoff_header.pl',
  output: 'daitch_mokotoff.h',
  command: [perl, '@INPUT@', '@OUTPUT@'],
)
generated_sources += daitch_mokotoff_h
fuzzystrmatch_sources += daitch_mokotoff_h

if host_system == 'windows'
  fuzzystrmatch_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'fuzzystrmatch',
    '--FILEDESC', 'fuzzystrmatch - similarities and distance between strings',])
endif

fuzzystrmatch = shared_module('fuzzystrmatch',
  fuzzystrmatch_sources,
  include_directories: include_directories('.'),
  kwargs: contrib_mod_args,
)
contrib_targets += fuzzystrmatch

install_data(
  'fuzzystrmatch.control',
  'fuzzystrmatch--1.0--1.1.sql',
  'fuzzystrmatch--1.1.sql',
  'fuzzystrmatch--1.1--1.2.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'fuzzystrmatch',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'fuzzystrmatch',
      'fuzzystrmatch_utf8',
    ],
  },
}
