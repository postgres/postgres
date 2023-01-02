# Copyright (c) 2022-2023, PostgreSQL Global Development Group

# FIXME: prevent install during main install, but not during test :/

test_slru_sources = files(
  'test_slru.c',
)

if host_system == 'windows'
  test_slru_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_slru',
    '--FILEDESC', 'test_slru - test module for SLRUs',])
endif

test_slru = shared_module('test_slru',
  test_slru_sources,
  kwargs: pg_mod_args,
)
testprep_targets += test_slru

install_data(
  'test_slru.control',
  'test_slru--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_slru',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_slru',
    ],
    'regress_args': ['--temp-config', files('test_slru.conf')],
    'runningcheck': false,
  },
}
