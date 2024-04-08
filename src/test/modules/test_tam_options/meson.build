# Copyright (c) 2024, PostgreSQL Global Development Group

test_tam_options_sources = files(
  'test_tam_options.c',
)

if host_system == 'windows'
  test_tam_options_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_tam_options',
    '--FILEDESC', 'test_tam_options -  test code for table access method reloptions',])
endif

test_tam_options = shared_module('test_tam_options',
  test_tam_options_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_tam_options

test_install_data += files(
  'test_tam_options.control',
  'test_tam_options--1.0.sql',
)

tests += {
  'name': 'test_tam_options',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_tam_options',
    ],
  },
}
