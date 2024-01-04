# Copyright (c) 2022-2024, PostgreSQL Global Development Group

plsample_sources = files(
  'plsample.c',
)

if host_system == 'windows'
  plsample_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'plsample',
    '--FILEDESC', 'PL/Sample - template for procedural language',])
endif

plsample = shared_module('plsample',
  plsample_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += plsample

test_install_data += files(
  'plsample.control',
  'plsample--1.0.sql',
)
tests += {
  'name': 'plsample',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'plsample',
    ],
  },
}
