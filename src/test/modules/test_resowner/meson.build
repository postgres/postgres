# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_resowner_sources = files(
  'test_resowner_basic.c',
  'test_resowner_many.c',
)

if host_system == 'windows'
  test_resowner_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_resowner',
    '--FILEDESC', 'test_resowner - test code for ResourceOwners',])
endif

test_resowner = shared_module('test_resowner',
  test_resowner_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_resowner

test_install_data += files(
  'test_resowner.control',
  'test_resowner--1.0.sql',
)

tests += {
  'name': 'test_resowner',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_resowner',
    ],
  },
}
