# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_predtest_sources = files(
  'test_predtest.c',
)

if host_system == 'windows'
  test_predtest_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_predtest',
    '--FILEDESC', 'test_predtest - test code for optimizer/util/predtest.c',])
endif

test_predtest = shared_module('test_predtest',
  test_predtest_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_predtest

test_install_data += files(
  'test_predtest.control',
  'test_predtest--1.0.sql',
)

tests += {
  'name': 'test_predtest',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_predtest',
    ],
  },
}
