# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_lfind_sources = files(
  'test_lfind.c',
)

if host_system == 'windows'
  test_lfind_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_lfind',
    '--FILEDESC', 'test_lfind - test code for optimized linear search functions',])
endif

test_lfind = shared_module('test_lfind',
  test_lfind_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_lfind

test_install_data += files(
  'test_lfind.control',
  'test_lfind--1.0.sql',
)

tests += {
  'name': 'test_lfind',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_lfind',
    ],
  },
}
