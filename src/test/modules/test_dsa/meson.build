# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_dsa_sources = files(
  'test_dsa.c',
)

if host_system == 'windows'
  test_dsa_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_dsa',
    '--FILEDESC', 'test_dsa - test code for dynamic shared memory areas',])
endif

test_dsa = shared_module('test_dsa',
  test_dsa_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_dsa

test_install_data += files(
  'test_dsa.control',
  'test_dsa--1.0.sql',
)

tests += {
  'name': 'test_dsa',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_dsa',
    ],
  },
}
