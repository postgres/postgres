# Copyright (c) 2026, PostgreSQL Global Development Group

test_custom_types_sources = files(
  'test_custom_types.c',
)

if host_system == 'windows'
  test_custom_types_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_custom_types',
    '--FILEDESC', 'test_custom_types - tests for dummy custom types',])
endif

test_custom_types = shared_module('test_custom_types',
  test_custom_types_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_custom_types

test_install_data += files(
  'test_custom_types.control',
  'test_custom_types--1.0.sql',
)

tests += {
  'name': 'test_custom_types',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_custom_types',
    ],
  },
}
