# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_ginpostinglist_sources = files(
  'test_ginpostinglist.c',
)

if host_system == 'windows'
  test_ginpostinglist_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_ginpostinglist',
    '--FILEDESC', 'test_ginpostinglist - test code for src/backend/access/gin//ginpostinglist.c',])
endif

test_ginpostinglist = shared_module('test_ginpostinglist',
  test_ginpostinglist_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_ginpostinglist

test_install_data += files(
  'test_ginpostinglist.control',
  'test_ginpostinglist--1.0.sql',
)

tests += {
  'name': 'test_ginpostinglist',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_ginpostinglist',
    ],
  },
}
