# Copyright (c) 2024, PostgreSQL Global Development Group

test_tidstore_sources = files(
  'test_tidstore.c',
)

if host_system == 'windows'
  test_tidstore_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_tidstore',
    '--FILEDESC', 'test_tidstore - test code for src/backend/access/common/tidstore.c',])
endif

test_tidstore = shared_module('test_tidstore',
  test_tidstore_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_tidstore

test_install_data += files(
  'test_tidstore.control',
  'test_tidstore--1.0.sql',
)

tests += {
  'name': 'test_tidstore',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_tidstore',
    ],
  },
}
