# Copyright (c) 2022-2023, PostgreSQL Global Development Group

# FIXME: prevent install during main install, but not during test :/

test_integerset_sources = files(
  'test_integerset.c',
)

if host_system == 'windows'
  test_integerset_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_integerset',
    '--FILEDESC', 'test_integerset - test code for src/backend/lib/integerset.c',])
endif

test_integerset = shared_module('test_integerset',
  test_integerset_sources,
  kwargs: pg_mod_args,
)
testprep_targets += test_integerset

install_data(
  'test_integerset.control',
  'test_integerset--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_integerset',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_integerset',
    ],
  },
}
