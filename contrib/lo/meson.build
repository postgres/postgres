# Copyright (c) 2022-2024, PostgreSQL Global Development Group

lo_sources = files(
  'lo.c',
)

if host_system == 'windows'
  lo_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'lo',
    '--FILEDESC', 'lo - management for large objects',])
endif

lo = shared_module('lo',
  lo_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += lo

install_data(
  'lo.control',
  'lo--1.0--1.1.sql',
  'lo--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'lo',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'lo',
    ],
  },
}
