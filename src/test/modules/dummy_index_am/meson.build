# Copyright (c) 2022-2024, PostgreSQL Global Development Group

dummy_index_am_sources = files(
  'dummy_index_am.c',
)

if host_system == 'windows'
  dummy_index_am_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dummy_index_am',
    '--FILEDESC', 'dummy_index_am - index access method template',])
endif

dummy_index_am = shared_module('dummy_index_am',
  dummy_index_am_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += dummy_index_am

test_install_data += files(
  'dummy_index_am.control',
  'dummy_index_am--1.0.sql',
)

tests += {
  'name': 'dummy_index_am',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'reloptions',
    ],
  },
}
