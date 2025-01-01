# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_custom_rmgrs_sources = files(
  'test_custom_rmgrs.c',
)

if host_system == 'windows'
  test_custom_rmgrs_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_custom_rmgrs',
    '--FILEDESC', 'test_custom_rmgrs - test custom WAL resource managers',])
endif

test_custom_rmgrs = shared_module('test_custom_rmgrs',
  test_custom_rmgrs_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_custom_rmgrs

test_install_data += files(
  'test_custom_rmgrs.control',
  'test_custom_rmgrs--1.0.sql',
)

tests += {
  'name': 'test_custom_rmgrs',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}
