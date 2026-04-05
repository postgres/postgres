# Copyright (c) 2024-2026, PostgreSQL Global Development Group

test_shmem_sources = files(
  'test_shmem.c',
)

if host_system == 'windows'
  test_shmem_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_shmem',
    '--FILEDESC', 'test_shmem - test code for shmem allocations',])
endif

test_shmem = shared_module('test_shmem',
  test_shmem_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_shmem

test_install_data += files(
  'test_shmem.control',
  'test_shmem--1.0.sql',
)

tests += {
  'name': 'test_shmem',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_late_shmem_alloc.pl',
    ],
  },
}
