# Copyright (c) 2024-2025, PostgreSQL Global Development Group

test_aio_sources = files(
  'test_aio.c',
)

if host_system == 'windows'
  test_aio_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_aio',
    '--FILEDESC', 'test_aio - test code for AIO',])
endif

test_aio = shared_module('test_aio',
  test_aio_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_aio

test_install_data += files(
  'test_aio.control',
  'test_aio--1.0.sql',
)

tests += {
  'name': 'test_aio',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_aio.pl',
      't/002_io_workers.pl',
    ],
  },
}
