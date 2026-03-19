# Copyright (c) 2026, PostgreSQL Global Development Group

test_saslprep_sources = files(
  'test_saslprep.c',
)

if host_system == 'windows'
  test_saslprep_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_saslprep',
    '--FILEDESC', 'test_saslprep - test SASLprep implementation',])
endif

test_saslprep = shared_module('test_saslprep',
  test_saslprep_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_saslprep

test_install_data += files(
  'test_saslprep.control',
  'test_saslprep--1.0.sql',
)

tests += {
  'name': 'test_saslprep',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_saslprep',
    ],
  },
  'tap': {
    'tests': [
      't/001_saslprep_ranges.pl',
    ],
  },
}
