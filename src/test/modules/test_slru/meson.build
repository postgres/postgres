# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_slru_sources = files(
  'test_slru.c',
  'test_multixact.c',
)

if host_system == 'windows'
  test_slru_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_slru',
    '--FILEDESC', 'test_slru - test module for SLRUs',])
endif

test_slru = shared_module('test_slru',
  test_slru_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_slru

test_install_data += files(
  'test_slru.control',
  'test_slru--1.0.sql',
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
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_multixact.pl'
    ],
  },
}
