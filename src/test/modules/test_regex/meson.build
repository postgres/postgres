# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_regex_sources = files(
  'test_regex.c',
)

if host_system == 'windows'
  test_regex_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_regex',
    '--FILEDESC', 'test_regex - test code for backend/regex/',])
endif

test_regex = shared_module('test_regex',
  test_regex_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_regex

test_install_data += files(
  'test_regex.control',
  'test_regex--1.0.sql',
)

tests += {
  'name': 'test_regex',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_regex',
      'test_regex_utf8',
    ],
  },
}
