# Copyright (c) 2022-2023, PostgreSQL Global Development Group

# FIXME: prevent install during main install, but not during test :/

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
  kwargs: pg_mod_args,
)
testprep_targets += test_custom_rmgrs

install_data(
  'test_custom_rmgrs.control',
  'test_custom_rmgrs--1.0.sql',
  kwargs: contrib_data_args,
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
