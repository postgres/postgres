# Copyright (c) 2026, PostgreSQL Global Development Group

test_extensible_sources = files(
  'test_extensible.c',
)

if host_system == 'windows'
  test_extensible_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_extensible',
    '--FILEDESC', 'test_extensible - test module for extensible node and custom scan',])
endif

test_extensible = shared_module('test_extensible',
  test_extensible_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_extensible

test_install_data += files(
  'test_extensible.control',
  'test_extensible--1.0.sql',
)

tests += {
  'name': 'test_extensible',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_extensible',
    ],
  },
}
