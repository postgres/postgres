# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_worker_spi_sources = files(
  'worker_spi.c',
)

if host_system == 'windows'
  test_worker_spi_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'worker_spi',
    '--FILEDESC', 'worker_spi - background worker example',])
endif

test_worker_spi = shared_module('worker_spi',
  test_worker_spi_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_worker_spi

test_install_data += files(
  'worker_spi.control',
  'worker_spi--1.0.sql',
)

tests += {
  'name': 'worker_spi',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_worker_spi.pl',
    ],
  },
}
